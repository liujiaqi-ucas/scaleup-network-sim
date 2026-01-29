#include "switch-mmu.h"

#include <fstream>
#include <iostream>

#include "ns3/assert.h"
#include "ns3/boolean.h"
#include "ns3/broadcom-node.h"
#include "ns3/double.h"
#include "ns3/global-value.h"
#include "ns3/log.h"
#include "ns3/object-vector.h"
#include "ns3/packet.h"
#include "ns3/random-variable.h"
#include "ns3/simulator.h"
#include "ns3/uinteger.h"
#include "flitheader.h"
#include "switch-node.h" // 【修改1】必须在这里 include，否则不能调用 m_node 的函数
NS_LOG_COMPONENT_DEFINE("SwitchMmu");
namespace ns3 {
TypeId SwitchMmu::GetTypeId(void) {
    static TypeId tid =
        TypeId("ns3::SwitchMmu")
            .SetParent<Object>()
            .AddConstructor<SwitchMmu>()
            // .AddAttribute(
            //     "MaxTotalBufferPerPort",
            //     "Maximum buffer size of MMU per port in bytes (12-port switch: 12 * 375kB = 4.5MB)",
            //     UintegerValue(375 * 1000),
            //     MakeUintegerAccessor(&SwitchMmu::SetMaxBufferBytesPerPort,
            //                          &SwitchMmu::GetMaxBufferBytesPerPort),
            //     MakeUintegerChecker<uint32_t>())
            .AddAttribute(
                "ActivePortCnt", "Number of active switch ports", UintegerValue(12),
                MakeUintegerAccessor(&SwitchMmu::SetActivePortCnt, &SwitchMmu::GetActivePortCnt),
                MakeUintegerChecker<uint32_t>());
    return tid;
}
SwitchMmu::SwitchMmu(void) {
    // Default buffer size: 375kB per active ports
    // 12-port switch: 12 * 375kB = 4.5MB
    // 32-port switch: 32 * 375kB = 12MB
    // m_maxBufferBytes = 4500 * 1000; //Originally: 9MB Current:4.5MB
    //m_uniform_random_var.SetStream(0);

    // dynamic threshold
    //m_dynamicth = false;
    // m_waitSpaceMap.resize(pCnt); 
    // m_waitPortMap.resize(pCnt);
    InitSwitch();
}
// 【修改3】实现 SetNode
void SwitchMmu::SetNode(SwitchNode* node) {
    m_node = node;
}
// 实现 RegisterWaitSpace
void SwitchMmu::RegisterWaitSpace(uint32_t outDev, uint32_t inDev) {
    // 记录：inDev 正在等待 outDev 的空间
    m_waitingForSpace[outDev].insert(inDev);
}
void SwitchMmu::RegisterWaitPort(uint32_t outDev, uint32_t inDev) {
    // 记录：inDev 正在等待 outDev 的空间
    m_waitingForLock[outDev].insert(inDev);
}
void SwitchMmu::Input(Ptr<Packet> p, uint32_t inDev) {
    uint32_t psize = p->GetSize();

    // 1. 【准入检查】(流控底线)
    // 只有一个 VC，qIndex 固定传 0
    if (!CheckIngressAdmission(inDev, 3, psize)) {
        //Drop(p); 
        //这应该直接报错，因为流控失效了
        NS_FATAL_ERROR("SwitchMmu::Input(): Ingress Admission Control Failed!");
        return;
    }

    // 2. 【物理入账】(占用共享内存)
    UpdateIngressAdmission(inDev, 3, psize);

    // 3. 【物理入队】(存入暂存队列)
    m_ingressQueues[inDev].push_back(p);
    std::cout<<"Node "<<m_node->GetId()<<" device "<<inDev<<"  m_ingressQueues  长度是"<<m_ingressQueues[inDev].size()<<std::endl;
     // 【核心修复】：如果当前端口被阻塞了，绝对不能尝试发送！
    if (m_ingressBlocked[inDev]) {
        // 既然阻塞了，说明队头那个包还没搞定，新来的包（无论是 Body 还是别的）
        // 都只能乖乖排队，等着被 ProcessIngressQueue 轮询到。
        return; 
    }
    // 4. 【初次推动】
    // 如果队列之前是空的，说明没人排队，我有机会直接走，赶紧试一下
    if (m_ingressQueues[inDev].size() == 1) {
        std::cout<<"switch "<<m_node->GetId()<<"的device"<<inDev<<" ingress队列只有我一个，我试一下直接转发"<<std::endl;
        ArbitrateAndSend(inDev);
    }
}


void SwitchMmu::ArbitrateAndSend(uint32_t inDev) {
    // 循环处理，直到队列空或被阻塞
    while (!m_ingressQueues[inDev].empty()) {
        
        // 1. 取队头
        Ptr<Packet> p = m_ingressQueues[inDev].front();
        FlitHeader fh;
        p->PeekHeader(fh);
        uint32_t type = fh.GetType();

        // 2. 尝试发送
        bool sent = m_node->AttemptForward(p, inDev);

        if (sent) {
            // ----------------------------------------------------
            // A. 发送成功
            // ----------------------------------------------------
            m_ingressQueues[inDev].pop_front();
            // 既然发成功了，blocked 肯定是 false
            m_ingressBlocked[inDev] = false; 

            // 处理连续发送逻辑
            if (type == 2 /*TAIL*/ || type == 3 /*SINGLE*/) {
                
                break; 
            }
            // 如果是 HEAD/BODY，必须继续发下一个（Wormhole 约束）
            continue; 

        } else {
            // ----------------------------------------------------
            // B. 发送失败 (阻塞)
            // ----------------------------------------------------
            // 既然发不出去，就标记阻塞
            // AttemptForward 内部已经调用了 RegisterWait...
            m_ingressBlocked[inDev] = true;
            
            // 退出循环！队头的包不走，后面的谁也别想走！
            // 这就完美防止了 Body 抢跑 HEAD 的问题。
            break; 
        }
    }
    
    return;
}
// 唤醒辅助函数
void SwitchMmu::WakeupIngress(uint32_t inDev) {
    // 只有当前确实是阻塞状态才唤醒
    // 否则可能是重复唤醒，或者该端口根本没货
    if (m_ingressBlocked[inDev]) {
        // 先解除封印
        m_ingressBlocked[inDev] = false;
        // 立即重试
        Simulator::ScheduleNow(&SwitchMmu::ArbitrateAndSend, this, inDev);
    }
}


void SwitchMmu::NotifyOutputPortFree(uint32_t outDev) {
    // Round-Robin 轮询：从上次服务位置的下一个开始查
    uint32_t start = (m_rrPtr[outDev] + 1) % m_activePortCnt;
    uint32_t curr = start;

    for (uint32_t i = 1; i <= m_activePortCnt; i++) {
        // 1. 如果这个入端口有包在排队
        // 【关键改进】先检查：这个入端口是否真的在等待这个出端口的锁？
        // 这样就避免了误唤醒那些等待 Credit 的人，或者刚来的人。
        // m_waitingForLock 是 std::set，count 很快
        if (m_waitingForLock[outDev].count(curr)) {
            
            // 双重保险：检查队列是否非空
            if (!m_ingressQueues[curr].empty()) {
                
                // 1. 移除等待名单 (必须先做)
                m_waitingForLock[outDev].erase(curr);

                // 2. 更新轮询指针 (公平性)
                m_rrPtr[outDev] = curr;

                // 3. 唤醒！(它会重置 blocked = false 并重试)
                WakeupIngress(curr);

                // 4. 找到一个接盘侠就退出，保证一次只放进来一个 HEAD
                return;
            } else {
                // 异常情况：它在名单里，但队列空了？可能是之前的逻辑 bug
                // 顺手清理掉
                m_waitingForLock[outDev].erase(curr);
            }
        }

        
        curr = (curr + 1) % m_activePortCnt;
    }
}



// 【新增】包级别流控检查
uint32_t SwitchMmu::CheckCreditRelease(uint32_t ingressPort, uint32_t qIndex, bool isTail) {
    
    
    return 1;
}
void SwitchMmu::InitSwitch(void) {
   
    NS_LOG_INFO("Static MMU Limit Per Queue: " << m_staticQueueLimitBytes);

    // 3. 清零计数器
    for (uint32_t i = 0; i < pCnt; i++) {
        for (uint32_t j = 0; j < qCnt; j++) {
            m_usedIngressPGBytes[i][j] = 0;
        }
    }
    for (uint32_t i = 0; i < pCnt; i++) {
        m_egressUsedBytes[i] = 0;
    }
    for (uint32_t i = 0; i < pCnt; i++) {
        m_egressCredits[i] = 2048; //初始每个出端口有2048个credit
    }
  for (uint32_t i = 0; i < pCnt; i++) {
    m_ingressBlocked[i] = false;
}
}

bool SwitchMmu::CheckIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize) {
    std::cout<<"Node "<<m_node->GetId()<<" 现在检查进包m_usedIngressPGBytes[ "<<port<<"]的值是"<<m_usedIngressPGBytes[port][qIndex]<<std::endl;
    // 【简单静态检查】
    if (m_usedIngressPGBytes[port][qIndex] + psize > m_staticQueueLimitBytes) {
        
        return false; // 满了，拒收
    }
    
    return true;
}


void SwitchMmu::UpdateIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize) {
    
    m_usedIngressPGBytes[port][qIndex] += psize;
    std::cout<<"Node "<<m_node->GetId()<<" 现在由于进包，更新后m_usedIngressPGBytes[ "<<port<<"]的值是"<<m_usedIngressPGBytes[port][qIndex]<<std::endl;
    //m_usedTotalBytes += psize;
}

void SwitchMmu::RemoveFromIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize) {
    
    if (m_usedIngressPGBytes[port][qIndex] >= psize) {
        m_usedIngressPGBytes[port][qIndex] -= psize;
        std::cout<<"Node "<<m_node->GetId()<<" 从ingress转发成功后，更新m_usedIngressPGBytes，现在port "<<port<<"的值是"<<m_usedIngressPGBytes[port][qIndex]<<std::endl;
    } else {
        NS_LOG_WARN("Underflow in Ingress Bytes!");
        m_usedIngressPGBytes[port][qIndex] = 0;
    }

    //if (m_usedTotalBytes >= psize) m_usedTotalBytes -= psize;
}
// 1. 准入检查：判断数组里的值是否超标
bool SwitchMmu::CheckEgressAdmission(uint32_t port) {
    // 简单逻辑：剩余的信用大于1，代表有一个信用就行,
    std::cout<<"switch Node "<<m_node->GetId()<<" device "<<port<<"的m_egressCredits["<<port<<"]是"<<m_egressCredits[port]<<std::endl;
    if (m_egressCredits[port] >=1) {
        return true;
    }
    return false;
}

// 2. 占用更新：转发成功时调用
void SwitchMmu::UpdateEgressAdmission(uint32_t port) {
    if (m_egressCredits[port] >0) {
        m_egressCredits[port] -= 1;
    
}
}

// 3. 释放更新：网卡发完包时调用
void SwitchMmu::ReleaseEgressAdmission(uint32_t outDev) {
    m_egressCredits[outDev]++;
    
    // 唤醒等待空间的人
    if (!m_waitingForSpace[outDev].empty()) {
        // 这里可以唤醒多个，或者只唤醒一个
        // 为安全起见，先唤醒一个
        uint32_t inDev = *m_waitingForSpace[outDev].begin();
        m_waitingForSpace[outDev].erase(m_waitingForSpace[outDev].begin());
        
        WakeupIngress(inDev);
    }
}


void SwitchMmu::ConfigNPort(uint32_t n_port) {
    m_activePortCnt = n_port;
    InitSwitch();
}

void SwitchMmu::ConfigBufferSize(uint32_t size) {
    // if size == 0, buffer size will be automatically decided
    //m_staticMaxBufferBytes = size;
    InitSwitch();
}

}  // namespace ns3
