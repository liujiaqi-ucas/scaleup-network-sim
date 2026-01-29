/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2006 Georgia Tech Research Corporation, INRIA
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Yuliang Li <yuliangli@g.harvard.com>
 */

#define __STDC_LIMIT_MACROS 1
#include "ns3/qbb-net-device.h"

#include <stdint.h>
#include <stdio.h>

#include <iostream>
#include <unordered_map>

#include "ns3/assert.h"
#include "ns3/boolean.h"
#include "ns3/cn-header.h"
#include "ns3/custom-header.h"
#include "ns3/data-rate.h"
#include "ns3/double.h"
#include "ns3/drop-tail-queue.h"
#include "ns3/error-model.h"
#include "ns3/flow-id-num-tag.h"
#include "ns3/flow-id-tag.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4.h"
#include "ns3/log.h"
#include "ns3/object-vector.h"
#include "ns3/pause-header.h"
#include "ns3/point-to-point-channel.h"
#include "ns3/pointer.h"
#include "ns3/ppp-header.h"
#include "ns3/qbb-channel.h"
#include "ns3/qbb-header.h"
#include "ns3/random-variable.h"
#include "ns3/rdma-hw.h"
#include "ns3/seq-ts-header.h"
#include "ns3/settings.h"
#include "ns3/simulator.h"
#include "ns3/udp-header.h"
#include "ns3/uinteger.h"
#include "switch-node.h"
#define MAP_KEY_EXISTS(map, key) (((map).find(key) != (map).end()))

NS_LOG_COMPONENT_DEFINE("QbbNetDevice");

namespace ns3 {

extern std::unordered_map<unsigned, Time> acc_pause_time;

// uint32_t RdmaEgressQueue::ack_q_idx = 3; // 3: Middle priority
uint32_t RdmaEgressQueue::ack_q_idx = 0;  // 0: high priority
// RdmaEgressQueue
TypeId RdmaEgressQueue::GetTypeId(void) {
    static TypeId tid =
        TypeId("ns3::RdmaEgressQueue")
            .SetParent<Object>()
            .AddTraceSource("RdmaEnqueue", "Enqueue a packet in the RdmaEgressQueue.",
                            MakeTraceSourceAccessor(&RdmaEgressQueue::m_traceRdmaEnqueue))
            .AddTraceSource("RdmaDequeue", "Dequeue a packet in the RdmaEgressQueue.",
                            MakeTraceSourceAccessor(&RdmaEgressQueue::m_traceRdmaDequeue));
    return tid;
}

RdmaEgressQueue::RdmaEgressQueue() {
    m_rrlast = 0;
    m_qlast = 0;
    
    m_ackQ = CreateObject<DropTailQueue>();
    m_ackQ->SetAttribute("MaxBytes",
                         UintegerValue(0xffffffff));  // queue limit is on a higher level, not here
}

Ptr<Packet> RdmaEgressQueue::DequeueQindex(int qIndex) {
    if (qIndex == -1) {  // high prio
        Ptr<Packet> p = m_ackQ->Dequeue();
        m_qlast = -1;
        m_traceRdmaDequeue(p, 0);
        return p;
    }
    if (qIndex >= 0) {                                          // qp
        Ptr<Packet> p = m_rdmaGetNxtPkt(m_qpGrp->Get(qIndex));  // 根据还剩多少现生成一个包
        m_rrlast = qIndex;
        m_qlast = qIndex;
        m_traceRdmaDequeue(p, m_qpGrp->Get(qIndex)->m_pg);
        return p;
    }
    return 0;
}
int RdmaEgressQueue::GetNextQindex() {  // 从队列对里面选一个队列，返回第一个满足所有条件的 QP
                                        // 索引，或者如果没有 QP 能发，返回 -1024（表示没东西可发）
    // 1. 绝对高优先级：ACK 队列
    // 如果 ACK 队列有包，必须优先让出，直接返回 -1
    if (m_ackQ->GetNPackets() > 0) return -1;

    uint32_t fcount = m_qpGrp->GetN();

    // 2. Round-Robin 轮询所有 QP
    for (uint32_t i = 1; i <= fcount; i++) {
        // 计算当前检查的 QP 索引 (从上次结束的位置开始)
        uint32_t curr = (i + m_rrlast) % fcount;

        // 3. 跳过已经结束的流
        if (m_qpGrp->IsQpFinished(curr)) continue;//这个看懂了

        Ptr<RdmaQueuePair> qp = m_qpGrp->Get(curr);


        // =======================================================
        // 条件 B: 数据检查 (Data Availability)
        // =======================================================
        // 只要有剩余字节没发完，就算有资格
        // 【修改点】：去掉了 IsWinBound 和 IRN 检查，只看有没有数据
        if (qp->GetBytesLeft() == 0) {
            // 如果没数据了，检查是否彻底结束
            if (qp->IsFinishedConst()) {
                m_qpGrp->SetQpFinished(curr);
            }
            continue;
        }

        // =======================================================
        // 条件 C: 物理层 Pacing (Inter-frame Gap)
        // =======================================================
        // 检查这个 QP 是否发得太快了，需要物理层冷却
        // 这里的 Simulator::Now() 比较的是纳秒级的时间戳
        if (qp->m_nextAvail.GetTimeStep() > Simulator::Now().GetTimeStep()) {//这个暂时还是存疑
            continue;  // 还在冷却中，跳过
        }

        // =======================================================
        // 结论：找到一个可以发送的 QP！
        // =======================================================
        // 注意：这里不检查 Credit。
        // Credit 检查由 Device 层的 DequeueAndTransmit 负责。
        // 如果这里返回了 curr，但在 Device 层发现没 Credit，
        // Device 层会直接 return，不进行实际发送，从而实现了信用流控。

        return curr;
    }

    // 找了一圈都没东西可发
    return -1024;
    
}

int RdmaEgressQueue::GetLastQueue() { return m_qlast; }

uint32_t RdmaEgressQueue::GetNBytes(uint32_t qIndex) {
    NS_ASSERT_MSG(qIndex < m_qpGrp->GetN(),
                  "RdmaEgressQueue::GetNBytes: qIndex >= m_qpGrp->GetN()");
    return m_qpGrp->Get(qIndex)->GetBytesLeft();
}

uint32_t RdmaEgressQueue::GetFlowCount(void) { return m_qpGrp->GetN(); }

Ptr<RdmaQueuePair> RdmaEgressQueue::GetQp(uint32_t i) { return m_qpGrp->Get(i); }



void RdmaEgressQueue::EnqueueHighPrioQ(Ptr<Packet> p) {
    m_traceRdmaEnqueue(p, 0);
    m_ackQ->Enqueue(p);
}

void RdmaEgressQueue::CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb) {
    while (m_ackQ->GetNPackets() > 0) {
        Ptr<Packet> p = m_ackQ->Dequeue();
        dropCb(p, 0);
    }
}

/******************
 * QbbNetDevice
 *****************/
NS_OBJECT_ENSURE_REGISTERED(QbbNetDevice);

TypeId QbbNetDevice::GetTypeId(void) {
    static TypeId tid =
        TypeId("ns3::QbbNetDevice")
            .SetParent<PointToPointNetDevice>()
            .AddConstructor<QbbNetDevice>()
            .AddAttribute("QbbEnabled", "Enable the generation of PAUSE packet.",
                          BooleanValue(true), MakeBooleanAccessor(&QbbNetDevice::m_qbbEnabled),
                          MakeBooleanChecker())
            .AddAttribute("QcnEnabled", "Enable the generation of PAUSE packet.",
                          BooleanValue(false), MakeBooleanAccessor(&QbbNetDevice::m_qcnEnabled),
                          MakeBooleanChecker())
            .AddAttribute("DynamicThreshold", "Enable dynamic threshold.", BooleanValue(false),
                          MakeBooleanAccessor(&QbbNetDevice::m_dynamicth), MakeBooleanChecker())
            .AddAttribute("PauseTime", "Number of microseconds to pause upon congestion",
                          UintegerValue(671),  // 65535*(64Bytes/50Gbps)
                          MakeUintegerAccessor(&QbbNetDevice::m_pausetime),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("TxBeQueue", "A queue to use as the transmit queue in the device.",
                          PointerValue(), MakePointerAccessor(&QbbNetDevice::m_queue),
                          MakePointerChecker<Queue>())
            .AddAttribute("RdmaEgressQueue", "A queue to use as the transmit queue in the device.",
                          PointerValue(), MakePointerAccessor(&QbbNetDevice::m_rdmaEQ),
                          MakePointerChecker<Object>())
            .AddTraceSource("QbbEnqueue", "Enqueue a packet in the QbbNetDevice.",
                            MakeTraceSourceAccessor(&QbbNetDevice::m_traceEnqueue))
            .AddTraceSource("QbbDequeue", "Dequeue a packet in the QbbNetDevice.",
                            MakeTraceSourceAccessor(&QbbNetDevice::m_traceDequeue))
            .AddTraceSource("QbbDrop", "Drop a packet in the QbbNetDevice.",
                            MakeTraceSourceAccessor(&QbbNetDevice::m_traceDrop))
            .AddTraceSource("RdmaQpDequeue", "A qp dequeue a packet.",
                            MakeTraceSourceAccessor(&QbbNetDevice::m_traceQpDequeue))
            .AddTraceSource("QbbPfc", "get a PFC packet. 0: resume, 1: pause",
                            MakeTraceSourceAccessor(&QbbNetDevice::m_tracePfc));

    return tid;
}

QbbNetDevice::QbbNetDevice() {
    NS_LOG_FUNCTION(this);
    m_ecn_source = new std::vector<ECNAccount>;
    for (uint32_t i = 0; i < qCnt; i++) {
        m_paused[i] = false;
    }
    for(int i=0;i<8;i++){
        m_ctrl_state[i]=CTRL_NONE;
    }
    // for (uint32_t i = 0; i < 8; i++) {
    //     m_ack_flags[i] = false;
    //     m_pending_acks[i] = 0;
    // }
    // 1. 初始化发送计数器
    m_txTotalSent = 0;
    m_is_retransmitting=0;
    // 2. 设定初始上限
    // 这个值有待商榷
    m_remoteBufferSize = 512;
    m_txLimit = m_remoteBufferSize;

    m_lastReceivedCreditLimit = 0;
    m_rxCumulativeFreed = 512;
    creditflag=false;

    m_rdmaEQ = CreateObject<RdmaEgressQueue>();

    
    //m_currentPacketReceivedFlits = 0;
    //m_currentPacketBytes = 0;
    //m_isReassembling = false;

    m_next_seq_num = 0;
    m_last_acked_seq = 0;
    expected_seq = 0;
    m_replay_trigger_seq = 0;
    m_currentleftsize = 0;
    m_totalFlits = 0;
    m_currentFlitIdx = 0;
}

QbbNetDevice::~QbbNetDevice() { NS_LOG_FUNCTION(this); }

void QbbNetDevice::DoDispose() {
    NS_LOG_FUNCTION(this);

    PointToPointNetDevice::DoDispose();
}

void QbbNetDevice::TransmitComplete(void) {
    NS_LOG_FUNCTION(this);
    NS_ASSERT_MSG(m_txMachineState == BUSY, "Must be BUSY if transmitting");
    m_txMachineState = READY;
    NS_ASSERT_MSG(m_currentPkt != 0, "QbbNetDevice::TransmitComplete(): m_currentPkt zero");
    m_phyTxEndTrace(m_currentPkt);
    m_currentPkt = 0;
    //std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"因为传输完成触发dequeue了"<<std::endl;
    DequeueAndTransmit();
}
//********************************************************************************************/
//
void QbbNetDevice::ReleaseRxCredit(uint16_t flitsFreed) {
    //std::cout<<"Node "<<m_node->GetId()<<"  device "<<m_ifIndex<<"执行了releaserxcredit函数，m_rxCumulativeFreed目前是"<<m_rxCumulativeFreed<<std::endl;
    Ptr<SwitchNode> swNode = DynamicCast<SwitchNode>(m_node);
    m_rxCumulativeFreed += flitsFreed;
    creditflag = true;  // 告诉device有新的credit要发送
    if(m_node->GetNodeType() == 1){
    std::cout<<"Node "<<m_node->GetId()<<"  device "<<m_ifIndex<<"执行了releaserxcredit函数，m_rxCumulativeFreed目前是"<<m_rxCumulativeFreed
    <<"目前我的ingress还有"<<swNode->m_mmu->m_ingressQueues[m_ifIndex].size()<<"个flit等待转发"
    <<std::endl;
    }else{
        std::cout<<"Node "<<m_node->GetId()<<"  device "<<m_ifIndex<<"执行了releaserxcredit函数，m_rxCumulativeFreed目前是"<<m_rxCumulativeFreed
    <<std::endl;
    }
     if (m_txMachineState == READY) {
         std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"因为有信用要释放要触发dequeue了"<<std::endl;
         DequeueAndTransmit();
     }
}
void QbbNetDevice::SendNextFlit() {  // 这个目前只是端侧的逻辑，
    
    NS_LOG_FUNCTION(this);

    // 0. 安全检查：如果没有大包在发，直接返回
    if (!m_currentLargePacket) {
        return;
    }

    // =========================================================
    // 1. 准备参数 (定义 Flit 结构)
    // =========================================================
    // 假设物理 Flit 大小是 256B，减去 FlitHeader(例如16B)，剩下是 Payload
    // 你需要根据你的 FlitHeader 实际大小来调整这个值
    const uint32_t MAX_FLIT_PAYLOAD_SIZE = 240; 
    
    // 获取大包总大小 (包含 IP/TCP/RDMA 等所有头部)
    uint32_t totalPacketSize = m_currentLargePacket->GetSize();
    uint16_t payloadsize=totalPacketSize-48;
    // 计算当前的偏移量 (基于当前是第几个 Flit)
    uint32_t currentOffset = m_currentFlitIdx * MAX_FLIT_PAYLOAD_SIZE;
    
    // 计算本次要切多少字节 (剩余不足 240 就切剩余的)
    uint32_t currentFragmentSize = std::min(MAX_FLIT_PAYLOAD_SIZE, 
                                            totalPacketSize - currentOffset);

    // =========================================================
    // 2. 【核心切片】CreateFragment (神技)
    // =========================================================
    // 这一步直接从大包里“抠”出一段字节。
    // 如果 offset=0 (Head Flit)，它自动包含了 IP/TCP 头部。
    // 如果 offset>0 (Body Flit)，它自动全是 Payload 数据。
    Ptr<Packet> flitPayload = m_currentLargePacket->CreateFragment(currentOffset, currentFragmentSize);

    // =========================================================
    // 3. 【分配序号】(用于 GBN/SR 重传)
    // =========================================================
    uint16_t seq = m_next_seq_num;
    m_next_seq_num++; 

    // =========================================================
    // 4. 【确定 Flit 类型】(自动判断，不再需要手动硬编码)
    // =========================================================
    FlitHeader fh;
    uint8_t flitType = 0;
    
    uint32_t nodeId = m_node->GetId();

    // 2. 获取当前设备索引 (Interface Index)
    uint32_t devIdx = m_ifIndex;
    bool isFirst = (currentOffset == 0);
    bool isLast  = (currentOffset + currentFragmentSize >= totalPacketSize);
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
                    
    if (isFirst && isLast) {
        flitPayload->PeekHeader(ch);
        flitType = 3; // SINGLE (既是头也是尾，包很小的情况)
        std::cout << "[PKT_SEND] Node=" << nodeId 
          << " Dev=" << devIdx  
          << " Seq=" << ch.udp.seq 
          << " Size=" << m_currentLargePacket->GetSize() 
          << " Flit seq="<<seq
          <<" 这是第"<<m_currentFlitIdx<<"个flit"
          << std::endl;
    } else if (isFirst) {
        flitType = 0; // HEAD (带 IP 头的)
        flitPayload->PeekHeader(ch);
        std::cout << "[PKT_SEND] Node=" << nodeId 
          << " Dev=" << devIdx  
          << " Seq=" << ch.udp.seq 
          << " Size=" << m_currentLargePacket->GetSize() 
          << " Flit seq="<<seq
          <<" 这是第"<<m_currentFlitIdx<<"个flit"
          << std::endl;
    } else if (isLast) {
        flitType = 2; // TAIL
        std::cout << "[PKT_SEND] Node=" << nodeId 
          << " Dev=" << devIdx   
          << " Size=" << m_currentLargePacket->GetSize() 
          << " Flit seq="<<seq
          <<" 这是第"<<m_currentFlitIdx<<"个flit"
          << std::endl;
    } else {
        flitType = 1; // BODY
        std::cout << "[PKT_SEND] Node=" << nodeId 
          << " Dev=" << devIdx   
          << " Size=" << m_currentLargePacket->GetSize() 
          << " Flit seq="<<seq
          <<" 这是第"<<m_currentFlitIdx<<"个flit"
          << std::endl;
    }

    // =========================================================
    // 5. 【加头】
    // =========================================================
    fh.SetType(flitType);
    fh.SetSeqNum(seq);
    // 只有 HEAD 才需要知道总共有几个 Flit (用于接收端预留空间/建立 Wormhole)
    if (flitType == 0 || flitType == 3) {
        // 向上取整计算总 Flit 数
        uint32_t totalFlits = (totalPacketSize + MAX_FLIT_PAYLOAD_SIZE - 1) / MAX_FLIT_PAYLOAD_SIZE;
        fh.SetPacketLen(totalFlits); 
    }
    fh.SetVcId(0); 
    fh.SetPktTotalBytes(payloadsize);
    std::cout<<"Node "<<m_node->GetId()<<"给第"<<seq<<"个flit加头，fh.SetPktTotalBytes = "<<payloadsize<<std::endl;
    // 把 FlitHeader 贴到切片前面
    flitPayload->AddHeader(fh);
    // =========================================================
    // 6. 【存入重传缓冲区】
    // =========================================================
    // flitPayload 此时包含了 [FlitHeader | Slice of Original Packet]
    // 必须 Copy，因为 TransmitStart 发送出去后，Packet 对象可能会被底层修改
    m_replayBuffer.Push(seq, flitPayload->Copy());

    // =========================================================
    // 7. 更新状态
    // =========================================================
    m_currentFlitIdx++;
    m_currentleftsize -= currentFragmentSize; // 保持你原有的逻辑，虽然有了 offset 其实可以不用它了

    // 如果切完了，释放大包指针，准备发下一个大包
    if (isLast || m_currentleftsize == 0) {
        m_currentLargePacket = nullptr;
        m_currentFlitIdx = 0; // 重置索引
        // 注意：m_next_seq_num 不重置，因为它是链路级的全局序号
    }

    // =========================================================
    // 8. 发送
    // =========================================================
    TransmitStart(flitPayload);
}

// 这个函数在 Receive 处理完逻辑后调用。它只负责更新记账，不负责具体发送
void QbbNetDevice::TriggerAck(uint8_t vc_id, uint16_t seq) {
    if (vc_id >= 8) return;  // 安全检查

    // 正常收到包，标记为需要 ACK
    // 注意：如果当前已经是 NAK 状态，说明之前已经报错了。
    // 但既然收到了正确的包（TriggerAck被调用），说明那个 NAK 已经被解决了（或者正在解决），
    // 这里的逻辑看你的 Receive 怎么写。通常 Receive 里收到正确包会覆盖 NAK 状态。
    
    m_ctrl_state[vc_id] = CTRL_ACK;
    m_ctrl_seq[vc_id] = seq;
    std::cout<<"Node "<<m_node->GetId()<<"的device"<<m_ifIndex<<"收到flit了，执行triggerack函数，此时ackseq是"<<seq<<std::endl;
    // ACK 不急，尝试唤醒发送队列看有没有顺风车
    if (m_txMachineState == READY) {
        //std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"因为有ack要发触发dequeue了"<<std::endl;
        DequeueAndTransmit();
    }
}
void QbbNetDevice::TriggerNak(uint8_t vc_id, uint16_t seq) {
    // 发现乱序，标记为 NAK
    m_ctrl_state[vc_id] = CTRL_NAK;
    m_ctrl_seq[vc_id] = seq;
    std::cout<<"Node "<<m_node->GetId()<<"收到乱序flit了，执行triggernak函数，此时naseq是"<<seq<<std::endl;
    // NAK 很急！即使没有数据要发，也得赶紧造一个控制包发出去
    // 这里可以直接调用 SendControlFlit，或者唤醒队列让 SendStandaloneControl 去处理
    if (m_txMachineState == READY) {
        // 方案一：立即插队发送（如果在单VC下不建议插队，则退化为方案二）
        // SendControlFlit(true, seq); 
        
        // 方案二：正常唤醒，依靠高优先级队列机制
        std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<" 因为有nak要发所以要dequeue了 "<<std::endl;
        DequeueAndTransmit(); 
    }
}
// 这个函数负责把 ACK "缝"到数据包上。它严格遵守 "同 VC 捎带" 原则
void QbbNetDevice::PiggybackAck(Ptr<Packet> p) {
    FlitHeader fh;
    p->PeekHeader(fh);
    uint8_t current_vc = fh.GetVcId();

    if (current_vc >= 8) return;

    // 检查状态机
    if (m_ctrl_state[current_vc] != CTRL_NONE) {
        std::cout<<" Node "<<m_node->GetId()<<"  device  "<<m_ifIndex<<"触发了piggybackack函数  ack序号是"<<m_ctrl_seq[current_vc];
        p->RemoveHeader(fh); // 剥离旧头

        // 填入序号
        if (m_ctrl_state[current_vc] == CTRL_NAK) {
            std::cout<<"是nak"<<std::endl;
            fh.SetNack(m_ctrl_seq[current_vc]); // 设置 NAK 标志和序号
            // NS_LOG_INFO("Piggybacking NAK " << m_ctrl_seq[current_vc]);
        } 
        else {
            std::cout<<"是ack"<<std::endl;
            fh.SetAck(m_ctrl_seq[current_vc]);  // 设置 ACK 标志和序号
            // NS_LOG_INFO("Piggybacking ACK " << m_ctrl_seq[current_vc]);
        }

        p->AddHeader(fh); // 加回新头

        // 关键：捎带完之后，状态清零
        // 哪怕是 NAK，只要发了一次，我就认为我已经通知对面了，
        // 没必要每个包都重复喊 NAK，除非又收到了新的乱序包触发了新的 TriggerNak
        m_ctrl_state[current_vc] = CTRL_NONE; 
    }
}
void QbbNetDevice::piggycredit(Ptr<Packet> p) {
    // 1. 获取当前数据包的 VC ID
    // 假设你使用 FlitHeader 或 CustomHeader，这里以 FlitHeader 为例
    FlitHeader fh;
    p->PeekHeader(fh);
    uint8_t current_vc = fh.GetVcId();

    // 安全检查
    if (current_vc >= 8) return;

    // 2. 检查：该 VC 是否有信用要回送
    if (creditflag) {
        //std::cout<<" Node "<<m_node->GetId()<<"  device  "<<m_ifIndex<<"触发了piggycredit函数  信用值是"<<m_rxCumulativeFreed<<std::endl;
        p->RemoveHeader(fh);
        
        fh.SetCredit(0, m_rxCumulativeFreed);  // 捎带credit字段
        p->AddHeader(fh);

        // 3. 销账
        creditflag = false;

        
    }
}
// 辅助函数：清空并重新填装
void QbbNetDevice::ReloadRetransQueue(uint16_t start_seq) {
    
    // 1. 清空当前剩余的重传任务 (这就相当于 指针回退/重置)
    m_retransQueue.clear();

    // 2. 从 ReplayBuffer 重新填装
    if (!m_replayBuffer.Empty()) {
        uint16_t head_seq = m_replayBuffer.GetFrontSeq();

        // 计算目标序号相对于队头的偏移量 (利用 ReplayBuffer 内置的序号回绕逻辑)
        // SeqDist(a, b) 返回 (a - b)
        int offset = ReplayBuffer::SeqDist(start_seq, head_seq);

        // 合法性检查：
        // offset < 0: 说明 start_seq 小于队头 (已经 ACK 过了)，无需处理
        // offset >= Size: 说明 start_seq 超过了 buffer 尾部 (还没发出去的包)，属于异常
        if (offset >= 0 && offset < (int)m_replayBuffer.Size()) {
            // 【核心优化】：利用 std::deque 迭代器的随机访问特性，O(1) 直接跳到起始位置
            auto it = m_replayBuffer.Begin() + offset;
            auto end = m_replayBuffer.End();

            // 遍历并将后续所有包加入 m_retransQueue
            for (; it != end; ++it) {
                // 注意：it->packet 是原始包的指针
                // 我们 push 一个 Copy() 进去，确保重传的包是独立的副本
                // 这在 ns-3 中开销很小 (COW 机制)，但能避免很多潜在的 Tag 冲突 bug
                if (it->packet) {
                    m_retransQueue.push_back(it->packet->Copy());
                }
            }
        } else {
            NS_LOG_WARN("ReloadRetransQueue: start_seq "
                        << start_seq << " is out of range (Head=" << head_seq
                        << ", Size=" << m_replayBuffer.Size() << "). Ignoring.");
        }
    }
    std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"填充重传队列完毕"<<std::endl;

    // 3. 更新状态
    // 只要填装了弹药，就标记为重传模式
    m_is_retransmitting = !m_retransQueue.empty();
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"装填重传队列成功，又进入重传模式了"<<std::endl;
    NS_LOG_INFO("ReloadRetransQueue: Queued " << m_retransQueue.size()
                                              << " packets starting from seq " << start_seq);

    // 4. 尝试发送
    // 如果物理层现在是空闲的，立即触发 DequeueAndTransmit 发射第一发
    // 如果物理层是 BUSY，会在 TransmitComplete 回调中自动处理，这里不用管
    if (m_is_retransmitting && m_txMachineState == READY) {
        DequeueAndTransmit();
    }
}

void QbbNetDevice::DequeueAndTransmit(void) {
    NS_LOG_FUNCTION(this);
    if (!m_linkUp) return;                 // if link is down, return
    if (m_txMachineState == BUSY) return;  // Quit if channel busy
    Ptr<Packet> p = nullptr;
    // ============================================================
    // 2. 端侧逻辑 (Server/NIC)
    // ============================================================
    if (m_node->GetNodeType() == 0) {
        // 先检查有没有没切完的包
        if (m_currentLargePacket != nullptr) {
            SendNextFlit();  // 切下一刀并发出去
            return;          // 直接返回，不要去调度新包
        }
        
        // 再检查重传队列
       if (!m_retransQueue.empty()) {  // 走到这里一定是新包
           p = m_retransQueue.front();
            m_retransQueue.pop_front();
            TransmitStart(p);  // 直接发送，反正就是把重传队列里面的都发完就对了
            
            NS_LOG_INFO("Switch Priority Sending Retransmission UID=" << p->GetUid());
            return;

        } else {
            // A. 询问调度器：下一个轮到谁？
            // 注意：GetNextQindex 只是计算，不会把包取出来，也不会改变 QP 状态
            int qIndex = m_rdmaEQ->GetNextQindex();

            if (qIndex != -1024) {  // 有队列需要发送 (ACK队列 还有 普通数据队列)

                uint32_t requiredFlits = 0;
                
                
                // ----------------------------------------------------
                // 优先级3：普通数据 QP (需要做严格的包级流控预判)
                // ----------------------------------------------------
                // 1. 获取 QP 句柄
                Ptr<RdmaQueuePair> qp = m_rdmaEQ->GetQp(qIndex);

                // 2. [预判] 计算如果现在产生包，这个包会有多大？
                // 逻辑必须与 RdmaHw::GetNxtPacket 保持一致：min(MTU, 剩余数据量)
                uint32_t mtu = m_rdmaEQ->m_mtu;
                uint32_t remaining = qp->GetBytesLeft();
                uint32_t payloadSize = std::min(mtu, remaining);
                
                // 估算协议头开销 (IP + UDP + IB + PPP 等)
                // 这是一个估算值，用于计算 Flit 数。建议稍微留点余量或根据实际抓包调整。
                // 假设头部总共约 60-80 字节。14+14+20=48
                uint32_t estimatedHeaderSize = 48;
                uint32_t totalSize = payloadSize + estimatedHeaderSize;//这个包算上头部总共的大小
                m_totalBytes = totalSize;
                // 计算需要的 Flit 数量 (向上取整)
                requiredFlits = (totalSize + 240 - 1) / 240;
                

                // 3. [核心] 包级流控检查
                uint16_t sent = (uint16_t)m_txTotalSent;
                uint16_t limit = m_txLimit;//

                // 2. 【关键】计算剩余信用 (利用无符号减法的回绕特性)
                uint16_t creditsLeft = limit - sent-requiredFlits;
                if (creditsLeft > 0 && creditsLeft < 32768) {
                    // >>>>>> 钱够了！允许产生包 >>>>>>

                    // a. 真正产生包 (此时 QP 的 snd_nxt 才会增加)真增加了吗，这个得去看看
                    Ptr<Packet> p = m_rdmaEQ->DequeueQindex(qIndex);  // 调用这个函数才会更新qp轮询的那个值
                    CustomHeader qw(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
                    p->PeekHeader(qw);
                    curpktpayload = p->GetSize() - qw.GetSerializedSize();  // 记录当前包的有效负载
                    m_txTotalSent += requiredFlits;  // 预先记账
                    std::cout<<" Node  "<<m_node->GetId()<<"的device "<<m_ifIndex<<"累计将要发送"<<m_txTotalSent<<"个flit"
                    <<"   下游目前让我发 "<<m_txLimit<<"个flit"<<std::endl;
                    // c. 记录 Trace
                    m_traceQpDequeue(p, qp);

                    // d. 更新 Pacing (UpdateNextAvail)
                    // 告诉 QP 这个包发完了，下次什么时候能再发
                    m_rdmaPktSent(qp, p, m_tInterframeGap);//这个真的有用吗,现在除了这个其他的都能确认了
                         // e. 启动切片状态机
                        m_currentLargePacket = p;
                        m_totalFlits = requiredFlits;  // 也可以用 p->GetSize() 再算一次更精确的
                        m_currentFlitIdx = 0;
                        m_currentleftsize = m_totalBytes;
                        SendNextFlit();
                        return;
                } 
                else {
                    std::cout<<" Node  "<<m_node->GetId()<<"的device "<<m_ifIndex<<"被信用阻塞了"<<std::endl;

                    // >>>>>> 钱不够！拒绝产生包 >>>>>>
                    //下一次dequeue会继续调用这个，不用担心卡死
                    // 什么都不做！不调用 DequeueQindex，QP 状态保持原样。
                    // 此时函数直接返回。
                    //
                    // 后果：
                    // 1. Round-Robin 指针不会更新。
                    // 2. 下次 GetNextQindex 还是会选中这个 QP。
                    // 3. 造成 Head-of-Line 阻塞，直到 Receive 函数收到 Credit 并唤醒
                    // DequeueAndTransmit。 这正是你要的“包级流控”效果。

                    //return;
                }
            }else{
                //代表没有东西要发送，那就得判断一下是不是又ack或者credit的需求了
                
            }
            std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"转发队列目前是空，我要看看有没有ack或者credit需求"<<std::endl;
         // 没包可发 (队列空 或 被流控阻塞)
        // 检查是否有欠下的 ACK 需要单独发送 (兜底机制)
        // 还得检查是不是有 credit 要发送
        bool ack_sent = false;
        // 遍历 8 个 VC，防止跨 VC 阻塞
        for (uint32_t i = 0; i < 8; i++) {
        
        // 检查两个条件：
        // 1. 是否有 ACK/NAK 要发 (m_ctrl_state)
        // 2. 是否有 Credit 要发 (creditflag) —— 即使没有 ACK，光发 Credit 也是有意义的
        
        bool need_ack_nak = (m_ctrl_state[i] != CTRL_NONE);
        
        // 如果有控制信息，或者 (作为优化) 刚好有 Credit 且这是一个活跃的 VC
        if (need_ack_nak) {
            std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"有控制信息要发"<<std::endl;
            NS_LOG_LOGIC("Creating Standalone Control Packet for VC " << i);

            // 1. 造空包 (Payload Size = 0)
             p = Create<Packet>(0);

            // 2. 准备头部
            FlitHeader fh;
            fh.SetType(3);   // TYPE = 3 (SINGLE)，表示单帧控制包
            fh.SetVcId(i);   // 填入对应的 VC
            fh.SetSeqNum(0); // 控制包不消耗 GBN 序号，填 0
            
            // 3. 填入 ACK / NAK 信息
            if (m_ctrl_state[i] == CTRL_NAK) {
                fh.SetNack(m_ctrl_seq[i]); // 标记为 NAK，填入期待序号
                NS_LOG_INFO("Sending Standalone NAK for VC " << i << " Seq " << m_ctrl_seq[i]);
            } 
            else if (m_ctrl_state[i] == CTRL_ACK) {
                fh.SetAck(m_ctrl_seq[i]);  // 标记为 ACK，填入确认序号
                NS_LOG_INFO("Sending Standalone ACK for VC " << i << " Seq " << m_ctrl_seq[i]);
            }

            // 4. 填入 Credit 信息 (顺便捎带)
            // 即使是专门发 ACK 的包，也别忘了把最新的信用带上
            if (creditflag) {
                std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"同时有流控信息也要发"<<std::endl;
                fh.SetCredit(0, m_rxCumulativeFreed); // 填入你的接收端释放计数
                creditflag = false; // 既然发出去了，标志位清零
            }

            // 5. 封包
            p->AddHeader(fh);

            // 6. 清除状态
            // 这个 ACK/NAK 已经随包发出了，任务完成
            m_ctrl_state[i] = CTRL_NONE;

            // 7. 发射！
            // TransmitStart 会设置 BUSY，并安排发送完成事件
            TransmitStart(p);
            
            // 重要：一次回调只发一个包。
            // 物理层变 BUSY 了，必须 return。
            // 等发完这个包，TransmitComplete 会再次调用 DequeueAndTransmit 处理剩下的。
            return; 
        }
    }
    
    // 补充情况：如果没有 ACK/NAK，但有 Credit 急需发送 (避免死锁)
    // 如果 creditflag 为 true，且上面循环没触发发送
    if (creditflag) {
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"目前只有信用需要单独发送，m_rxCumulativeFreed是   "<<m_rxCumulativeFreed<<std::endl;
        // 随便找一个 VC (通常是 0) 发送纯 Credit Update
        p = Create<Packet>(0);
        FlitHeader fh;
        fh.SetType(3);
        fh.SetVcId(0);
        fh.SetCredit(0, m_rxCumulativeFreed);
        p->AddHeader(fh);
        
        creditflag = false;
        TransmitStart(p);
        return;
    }
    return;
        }
    }  // =========================================================
       // 场景 2: Switch (交换机) - 【融合流控版】
       // =========================================================
    else {
        if (m_switchPendingPkt != nullptr) {
            std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"目前有暂存的包"<<std::endl;
        // 1.1 再次验资 (Check)
        FlitHeader fh;
        m_switchPendingPkt->PeekHeader(fh);
        uint32_t required = fh.GetPacketLen();
        
        // 计算剩余可用信用
        uint16_t creditsLeft = m_txLimit - m_txTotalSent;

        if (creditsLeft >= required) {
            // [通过]：有钱了！
            p = m_switchPendingPkt;          // 取回包
            m_switchPendingPkt = nullptr;    // 清空暂存区
            
            // [预扣除]：Head 一次性扣掉整包的信用
            m_txTotalSent += required;
            
            // 跳转到公共发送逻辑（去打 SeqNum）
            goto PROCESS_NEW_PACKET;
        } else {
            std::cout<<" switch Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"发消息被阻塞了，目前我的m_txLimit是"<<m_txLimit<<"   我的信用是"<<m_txTotalSent<<std::endl;
            // [失败]：还是没钱，继续卡着，直接返回
            return; 
        }
    }

    // =================================================================
    // 2. 【次高优先级】重传队列 (Local Retransmission)
    //    重传包已经是成品（有 SeqNum），不需要再 Check 信用（假设享有特权）
    // =================================================================
    if (!m_retransQueue.empty()) {
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"重传队列有包要发"<<std::endl;
        p = m_retransQueue.front();
        m_retransQueue.pop_front();
        
        NS_LOG_INFO("Switch Resending UID=" << p->GetUid());
        TransmitStart(p); // 直接发，不打新 Seq，不进 ReplayBuffer
        return;
    }

    // =================================================================
    // 3. 【正常调度】从转发队列取新包
    // =================================================================
    if (m_queue->IsEmpty()) {//说明现在转发队列没有包，但是我要检查有没有需要ack或者credit的需求，我要单独造一个包
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"转发队列目前是空，我要看看有没有ack或者credit需求"<<std::endl;
         // 没包可发 (队列空 或 被流控阻塞)
        // 检查是否有欠下的 ACK 需要单独发送 (兜底机制)
        // 还得检查是不是有 credit 要发送
        bool ack_sent = false;
        // 遍历 8 个 VC，防止跨 VC 阻塞
        for (uint32_t i = 0; i < 8; i++) {
        
        // 检查两个条件：
        // 1. 是否有 ACK/NAK 要发 (m_ctrl_state)
        // 2. 是否有 Credit 要发 (creditflag) —— 即使没有 ACK，光发 Credit 也是有意义的
        
        bool need_ack_nak = (m_ctrl_state[i] != CTRL_NONE);
        
        // 如果有控制信息，或者 (作为优化) 刚好有 Credit 且这是一个活跃的 VC
        if (need_ack_nak) {
            std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"有控制信息要发"<<std::endl;
            NS_LOG_LOGIC("Creating Standalone Control Packet for VC " << i);

            // 1. 造空包 (Payload Size = 0)
             p = Create<Packet>(0);

            // 2. 准备头部
            FlitHeader fh;
            fh.SetType(3);   // TYPE = 3 (SINGLE)，表示单帧控制包
            fh.SetVcId(i);   // 填入对应的 VC
            fh.SetSeqNum(0); // 控制包不消耗 GBN 序号，填 0
            
            // 3. 填入 ACK / NAK 信息
            if (m_ctrl_state[i] == CTRL_NAK) {
                fh.SetNack(m_ctrl_seq[i]); // 标记为 NAK，填入期待序号
                NS_LOG_INFO("Sending Standalone NAK for VC " << i << " Seq " << m_ctrl_seq[i]);
            } 
            else if (m_ctrl_state[i] == CTRL_ACK) {
                fh.SetAck(m_ctrl_seq[i]);  // 标记为 ACK，填入确认序号
                NS_LOG_INFO("Sending Standalone ACK for VC " << i << " Seq " << m_ctrl_seq[i]);
            }

            // 4. 填入 Credit 信息 (顺便捎带)
            // 即使是专门发 ACK 的包，也别忘了把最新的信用带上
            if (creditflag) {
                std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"同时有流控信息也要发"<<std::endl;
                fh.SetCredit(0, m_rxCumulativeFreed); // 填入你的接收端释放计数
                creditflag = false; // 既然发出去了，标志位清零
            }

            // 5. 封包
            p->AddHeader(fh);

            // 6. 清除状态
            // 这个 ACK/NAK 已经随包发出了，任务完成
            m_ctrl_state[i] = CTRL_NONE;

            // 7. 发射！
            // TransmitStart 会设置 BUSY，并安排发送完成事件
            TransmitStart(p);
            
            // 重要：一次回调只发一个包。
            // 物理层变 BUSY 了，必须 return。
            // 等发完这个包，TransmitComplete 会再次调用 DequeueAndTransmit 处理剩下的。
            return; 
        }
    }
    
    // 补充情况：如果没有 ACK/NAK，但有 Credit 急需发送 (避免死锁)
    // 如果 creditflag 为 true，且上面循环没触发发送
    if (creditflag) {
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"目前只有信用需要单独发送，m_rxCumulativeFreed是   "<<m_rxCumulativeFreed<<std::endl;
        // 随便找一个 VC (通常是 0) 发送纯 Credit Update
        p = Create<Packet>(0);
        FlitHeader fh;
        fh.SetType(3);
        fh.SetVcId(0);
        fh.SetCredit(0, m_rxCumulativeFreed);
        p->AddHeader(fh);
        
        creditflag = false;
        TransmitStart(p);
        return;
    }
    return;
    }
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"目前转发队列是有包要发的"<<std::endl;
    //下面的情况就说有正常包需要发送的情况了
    // 3.1 取包 (先斩)
    {
        bool dummy_paused[8] = {false};
        p = m_queue->DequeueRR(dummy_paused);
    }
    
    if (p == nullptr) return;

    // 3.2 识别与验资 (后奏)
    { 
        FlitHeader fh;
        p->PeekHeader(fh); // 偷看一眼头部信息
        uint32_t type = fh.GetType();

        // >>> 情况 A: 是 HEAD 或 SINGLE (需要 VCT 验资) >>>
        if (type == 0 || type == 3) {
            uint32_t required = fh.GetPacketLen();
            uint16_t creditsLeft = m_txLimit - m_txTotalSent;

            if (creditsLeft >= required) {
                // [通过]：预扣除信用
                m_txTotalSent += required;
                // 继续向下走，去打 SeqNum 发送
            } else {
                // [失败]：没钱！
                // 必须把包存入“暂存区”，并卡住流水线
                m_switchPendingPkt = p;
                // m_switchPendingQIndex = qIndex; // 如果需要
                return; // 结束，等待 ProcessAck 带来信用
            }
        } 
        // >>> 情况 B: 是 BODY 或 TAIL (无条件放行) >>>
        else {
            // 逻辑闭环：
            // 因为 Head 发送时已经执行了 m_txTotalSent += Total (预扣除)
            // 所以 Body 发送时不需要再扣 m_txTotalSent，否则就重复扣费了。
            // 直接放行。
        }
    }

PROCESS_NEW_PACKET:
    // =================================================================
    // 4. 新包统一处理 (分配序号 + 备份 + 发送)
    // =================================================================
    
    // 4.1 更新头部：打上链路级 SeqNum
    FlitHeader fh;
    p->RemoveHeader(fh);           // 1. 取下旧头 (端侧的或上一跳的)
    fh.SetSeqNum(m_next_seq_num);  // 2. 更新为本端口的发送序号
    m_next_seq_num++;              // 3. 指针自增
    p->AddHeader(fh);              // 4. 装回新头
    //做一些统计
    m_snifferTrace(p);
    m_promiscSnifferTrace(p);
    FlowIdTag t;
    uint32_t qIndex = m_queue->GetLastQueue();    
    m_node->SwitchNotifyDequeue(m_ifIndex, qIndex, p);
    p->RemovePacketTag(t);
    m_traceDequeue(p, qIndex);
    //
    // 4.2 存入重传缓冲区 (必须存 Copy)
    // 这里的 fh.GetSeqNum() 就是刚才设置的 m_next_seq_num
    m_replayBuffer.Push(fh.GetSeqNum(), p->Copy());

    // 4.3 通知上层 (如果你的 SwitchNode 需要统计或处理)
     Ptr<SwitchNode> swNode = DynamicCast<SwitchNode>(m_node);
     if(swNode){
    swNode->releasecredit(m_ifIndex);//这里是向ingress释放一个空间
     }
    // 4.4 发射
    TransmitStart(p);
        
        //能走到这里的都是没有包可以发送的情况
    }
    // =========================================================
    // 最终决策：发送 (捎带 ACK) OR 兜底 (纯 ACK)
    // =========================================================

    
        
}

//**************************************************************************************** */
void QbbNetDevice::ProcessAck(uint16_t ack_seq) {
    NS_LOG_FUNCTION(this << ack_seq);

    // ==========================================
    // 1. 合法性检查 (Sanity Check)
    // ==========================================
    // 防止重复 ACK 或过期 ACK 扰乱逻辑
    // m_last_acked_seq 建议直接从 m_replayBuffer 获取最老的未确认序号之前的那个
    // 或者维护一个成员变量。这里假设你维护了 m_last_acked_seq。
    if (!ReplayBuffer::IsNewer(ack_seq, m_last_acked_seq)) {
        return;
    }

    // ==========================================
    // 2. 仓库清理 (Clean the Warehouse)
    // ==========================================
    // 调用 ReplayBuffer 封装好的 Ack 接口，移除已确认的副本
    // 这会自动释放窗口空间
    uint32_t removed_count = m_replayBuffer.Ack(ack_seq);

    // 更新最后确认的序号
    m_last_acked_seq = ack_seq;

    // ==========================================
    // 3. 生产线清理 (Prune the Retransmission Queue)
    // ==========================================
    // 这是替代原先 "Fast Forward" 的逻辑。
    // 如果重传队列里还有包，我们需要检查它们是不是已经被刚才这个 ACK 覆盖了。
    // 如果覆盖了，就直接扔掉，不用再重传了。

    while (!m_retransQueue.empty()) {
        // 1. 偷看队头 (Peek)
        Ptr<Packet> p = m_retransQueue.front();

        // 2. 提取序号
        // 注意：这里假设 Packet 里已经带了 FlitHeader
        FlitHeader fh;
        p->PeekHeader(fh);
        uint16_t p_seq = fh.GetSeqNum();

        // 3. 判断：如果包的序号 <= ack_seq，说明在这个 ACK 之前，或者就是这个 ACK
        // 利用 ReplayBuffer 提供的回绕比较逻辑
        if (ReplayBuffer::IsLeq(p_seq, ack_seq)) {
            // 已被确认，不需要重传了，扔掉！
            m_retransQueue.pop_front();
            NS_LOG_INFO("ProcessAck: Pruning seq " << p_seq
                                                   << " from retrans queue (Fast Forward)");
        } else {
            // 队头序号 > ack_seq，说明这个包还没被确认，必须保留
            // 因为队列是按序号排序的，后面肯定也都更大，所以直接 break
            break;
        }
    }

    // ==========================================
    // 4. 状态更新
    // ==========================================
    // 如果队列被清空了，说明重传任务全部完成（或者被 ACK 取消了）
    if (m_retransQueue.empty()) {
        if(m_is_retransmitting){
        std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"从重传状态恢复成正常状态了"<<std::endl;
        }
        m_is_retransmitting = false;
        // 注意：不要清空 m_last_nack_seq，那个是用来防 NACK 抖动的，跟 ACK 没关系
    }

    
}
void QbbNetDevice::processnak(uint16_t nack_seq) {
    // 1. 合法性检查：如果是已经确认过的包，或者是当前 Ack 的包，忽略
    // (nack_seq 必须在 last_acked 的未来)
    std::cout<<"Node "<<m_node->GetId()<<"device "<<m_ifIndex<<"进入处理nak的过程了"<<"现在的m_is_retransmitting是 "<<m_is_retransmitting<<std::endl;
    if (!m_replayBuffer.IsNewer(nack_seq, m_last_acked_seq)) {
        return;
    }

    // 2. 分状态处理
    if (m_is_retransmitting && !m_retransQueue.empty()) {
        // --- 重传模式下 (抗干扰逻辑) ---

        // 情况 A: 重复的 NACK (最常见)
        // 比如针对 seq 10 来了 5 个 NACK，只有第 1 个进下面的 else，后 4 个都在这里 return
        if (nack_seq == m_replay_trigger_seq) {
             std::cout<<"Node "<<m_node->GetId()<<"device "<<m_ifIndex<<"在重传模式下收到重复ack了"<<std::endl;
            return;
        }

        // 情况 B: 再次回退 (Nested Loss)
        // 只有当 nack 介于 Trigger 和 ReplayPtr 之间时，才说明重传的数据又丢了
        // 判断：nack_seq > Trigger 且 nack_seq < ReplayPtr (注意处理回绕)
        std::cout<<"Node "<<m_node->GetId()<<"device "<<m_ifIndex<<"在重传模式下重传的包又丢了"<<std::endl;
        uint16_t next_to_send = 0;
        FlitHeader fh;
        m_retransQueue.front()->PeekHeader(fh);
        next_to_send = fh.GetSeqNum();
        if (m_replayBuffer.IsNewer(nack_seq, m_replay_trigger_seq)) {
            NS_LOG_INFO("Nested Loss detected! Resetting retrans from " << nack_seq);

            // 【关键动作】：指针回退 = 队列刷新
            // 扔掉当前的 [105, 106...]，重新装填 [103, 104, 105...]
            ReloadRetransQueue(nack_seq);
            m_replay_trigger_seq = nack_seq;  // 更新案底
        }

        // 情况 C: 如果 nack_seq >= m_replay_ptr，说明是还没重传到的地方，
        // 或者是接收端逻辑错乱，通常忽略。
    } else {
        // --- 正常模式下 (立即响应) ---
        std::cout<<"Node "<<m_node->GetId()<<"device "<<m_ifIndex<<"在正常模式收到nak立马进入重传"<<std::endl;
        NS_LOG_INFO("New Retransmission Triggered at " << nack_seq);

        // 1. 装填弹药
        ReloadRetransQueue(nack_seq);

        // 3. 记录触发源 (设防)
        m_replay_trigger_seq = nack_seq;
    }
}
//**************************************************************************** */
void QbbNetDevice::SendControlFlit(bool is_nack, uint16_t seq) {
    NS_LOG_FUNCTION(this << is_nack << seq);

    // 1. 造一个纯控制包 (无 Payload)
    // 根据我们刚才的讨论，大小为 0，加上头就是 16 字节
    Ptr<Packet> p = Create<Packet>(0);

    // 2. 填充头部
    FlitHeader fh;
    fh.SetType(3);    // 3 = SINGLE (通常用于单帧控制包)
    fh.SetVcId(0);    // 单 VC 模式默认 0
    fh.SetSeqNum(0);  // 控制包本身不占用 GBN 序号，填 0 即可

    // 设置关键信息
    if (is_nack) {
        fh.SetNack(seq);  // 设置 NACK 标记和序号
    } else {
        fh.SetAck(seq);  // 设置 ACK 标记和序号
    }

    p->AddHeader(fh);

    // 3. 发送策略 (插队)
    // 我们希望这个包立刻出去，不要排在几百个数据包后面

    if (m_txMachineState == READY) {
        // 链路空闲，直接获得物理层控制权发射！
        // 注意：这里直接调用 TransmitStart，跳过了 DequeueAndTransmit 的队列检查
        TransmitStart(p);
    } else {
        // 链路忙，必须排队。但我们要插到"VIP通道"里。

        if (m_node->GetNodeType() == 0) {  // Server
            // 插到 ACK 专用高优先级队列
            RdmaEnqueueHighPrioQ(p);//插入到专门的高优先级队列
        } else {  // Switch
            // 插到 0 号队列 (绝对高优先级)
            m_queue->Enqueue(p, 0);
        }
        // 注意：这里不需要显式调用 DequeueAndTransmit，因为 TransmitComplete 回调会来取
    }
}
//*****************************************************************************/
void QbbNetDevice::Receive(Ptr<Packet> packet) {
    NS_LOG_FUNCTION(this << packet);
    if (!m_linkUp) {
        m_traceDrop(packet, 0);
        return;
    }

    if (m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet)) {
        //
        // If we have an error model and it indicates that it is time to lose a
        // corrupted packet, don't forward this packet up, let it go.
        //
        m_phyRxDropTrace(packet);
        std::cout << "Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<" 丢包了 " 
                  << std::endl;  // 丢弃包的时候打印一下日志
        return;
    }
    int packetsize=packet->GetSize();
    m_macRxTrace(packet);
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    //ch.getInt = 1;  // parse INT header
    // packet->PeekHeader(ch);
    // 1. 准备一个空的 Header 对象（容器）
    FlitHeader fh;

    // 2. 【关键步骤】偷看头部 (Peek)
    // ns-3 会读取 packet 前 16 字节，填充到 fh 对象里
    // 注意：此时 p 内部的数据并没有被移除，p 还是完整的
    uint32_t bytes_read = packet->RemoveHeader(fh);//注意，这里我把flit的头部先给移除，为了改几个字段
    // 校验：确保真的读到了头 (防御性编程)
    if (bytes_read < fh.GetSerializedSize()) {
        NS_LOG_ERROR("Packet too small to contain FlitHeader!");
        return;
    }
    // 3. 现在可以提取序号了
    uint16_t seq = fh.GetSeqNum();
    uint8_t vc = fh.GetVcId();
    uint8_t type = fh.GetType();
    uint32_t  payloadsize=fh.GetPktTotalBytes();
    std::cout<<"Node "<<m_node->GetId()<<"收到的flit头部携带的pkttotalbytes大小是"<<payloadsize<<std::endl;
    // ---------------------------------------------------------
    // 第一步：无条件提取捎带信息 (Process Piggyback FIRST)
    // ---------------------------------------------------------
    // 无论这个 Flit 是数据还是控制，是乱序还是重复，
    // 它携带的 "对方对我的确认" 和 "对方给我的流控" 都是宝贵的。

    // 1. 处理对方捎带过来的 ACK/NACK
    if (fh.HasAck()) {  // 如果有捎带ack/nak的话
        if (fh.IsNack()) {
            std::cout<<"Node "<<m_node->GetId()<<"收到了nak，序号是"<<fh.GetAckSeq()<<std::endl;
            // 对方报错了，我作为发送端需要回退重传
            // 这里需要有一个很重要的重传函数
            processnak(fh.GetAckSeq());
        } else {
            // 对方确认了，清理我的重传缓冲区
            // 这里会调用一个重传缓冲区的清理函数
            std::cout<<"Node "<<m_node->GetId()<<"收到了ack，序号是"<<fh.GetAckSeq()<<std::endl;
            ProcessAck(fh.GetAckSeq());
        }
        fh.setackflag(0);//收到捎带之后需要把这个捎带标记清除，要不然可能会让下游产生误解
    }

    if (fh.HasCredit()) {
        fh.setcreditflag(0);
        uint16_t currentCredit = fh.GetCreditLimit();
        m_txLimit = currentCredit;
        std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到了下游的流控回复   "<<"现在的信用限制是"<<m_txLimit<<std::endl;
        // // 利用 uint16_t 的回绕特性计算增量 (Delta)
        // // 例如：上次 65530，这次 5。 5 - 65530 = 11。
        // uint16_t delta = currentCredit - m_lastReceivedCreditLimit;

        // if (delta > 0) {
        //     // 更新 64位 的总发送上限
        //     m_txLimit += delta;

        //     // 记录当前值供下次计算
        //     m_lastReceivedCreditLimit = currentCredit;

        //     // 【关键】唤醒发送机
        //     // 如果之前因为没钱 (m_txTotalSent >= m_txLimit) 卡住了，现在有钱了，赶紧尝试发送
        //std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"因为收到流控要触发dequeue了"<<std::endl;
        DequeueAndTransmit();  // 有信用了重试一下发送
        //     }
        // }
    }

    // ---------------------------------------------------------
    // 第二步：区分 Flit 类型
    // ---------------------------------------------------------
    if (packetsize == fh.GetSerializedSize()) {
        // 这是一个纯控制包（比如专门发的 NACK 或 Credit Update）
        // 它的 Payload 无效，且不占用 GBN 序号
        // 任务在第一步已经完成了，直接结束
        return;
    }

    // ---------------------------------------------------------
    // 第三步：数据包 GBN 核心判断 (The Bouncer)
    // ---------------------------------------------------------

    // 计算距离 (处理回绕)
    int dist = SeqDist(seq, expected_seq);

    // --- 情况 A: 正好是我想要的 (Hit) ---
    if (dist == 0) {
        // 1. 接收数据
        // ProcessPayload(incoming_flit);
        TriggerAck(0, expected_seq);  // 触发ack机制，告诉对方我收到了expected_seq
        // 2. 推进窗口
        expected_seq += 1;
        if(m_node->GetNodeType() == 0){//如果是交换机测的话，立马回送一个信用就行
            std::cout<<"server Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"要触发release函数了 "<<std::endl;
             ReleaseRxCredit(1);
        }
        // 检查是否能附着，能附着的话就附着，不能附着的话单独生成一个ackflit

    }

    // --- 情况 B: 是未来的包 (Gap Detected) ---
    else if (dist > 0) {
        // 1. 丢弃数据 (GBN 不缓存乱序包)
        // Drop(incoming_flit);

        
       TriggerNak(0, expected_seq);
       return;
    }

    // --- 情况 C: 是过去的旧包 (Duplicate) ---
    else {  // dist < 0
        // 1. 丢弃数据 (已经处理过了)
        // Drop(incoming_flit);

        // 2. 重发 ACK (Re-ACK)
        // 对面可能没收到之前的 ACK 导致超时重传了
        // 我必须再次大声告诉它："在这个序号之前的我都收到了！"
        // 这里的 ACK 序号应该是 (expected_seq - 1)
        TriggerAck(0, expected_seq);
        return;
    }
     packet->AddHeader(fh);//再把改过的头部加回来
    if (m_node->GetNodeType() > 0) {  // switch
                                      // 【关键修改】
        // 交换机需要完整的 Flit (带 FlitHeader) 才能转发给下一跳。
        // 所以这里 **不要 RemoveHeader(fh)**！
        // 也不要尝试在这里解析 CustomHeader (因为隔着 FlitHeader 解析不到)。

        // CustomHeader dummyCh; // 传个空的进去，让 SwitchNode 自己去解析
        packet->AddPacketTag(FlowIdTag(
            m_ifIndex));  // 反正就是把入端口的标签加进去了，知道这个包是从哪个端口进来的了
        // m_node->SwitchReceiveFromDevice(this, packet, ch);
        //  把“带皮”的包扔给 SwitchNode
        m_node->SwitchReceiveFromDevice(this, packet, ch);
    } else {  // NIC

        // send to RdmaHw
        int ret = m_rdmaReceiveCb(packet, ch);
        // TODO we may based on the ret do something
        //if (ret == 0) DoMpiReceive(packet);//这个我先给注释了，好像对我没啥影响
    }
    return;
}

bool QbbNetDevice::Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber) {
    NS_ASSERT_MSG(false, "QbbNetDevice::Send not implemented yet\n");
    return false;
}

bool QbbNetDevice::SwitchSend(uint32_t qIndex, Ptr<Packet> packet,CustomHeader &ch) {
    Ptr<Queue> subQueue = m_queue->GetQueue(qIndex); 
    std::cout<<" switchsend"<<std::endl;
   // 2. 进行类型转换
     Ptr<DropTailQueue> dt = DynamicCast<DropTailQueue>(subQueue);
    m_macTxTrace(packet);
    m_traceEnqueue(packet, qIndex);
    m_queue->Enqueue(packet, qIndex);
    std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"的egress队列长度是"<<dt->GetNPackets()<<std::endl;
    std::cout<<"Node "<<m_node->GetId()<<"的device "<<m_ifIndex<<"刚刚入队了一个包，马上就要触发dequeue函数了"<<std::endl;
    DequeueAndTransmit();
    return true;
}

bool QbbNetDevice::Attach(Ptr<QbbChannel> ch) {
    NS_LOG_FUNCTION(this << &ch);
    m_channel = ch;
    m_channel->Attach(this);
    NotifyLinkUp();
    return true;
}

bool QbbNetDevice::TransmitStart(Ptr<Packet> p) {
    NS_LOG_FUNCTION(this << p);
    NS_LOG_LOGIC("UID is " << p->GetUid() << ")");
    //
    // This function is called to start the process of transmitting a packet.
    // We need to tell the channel that we've started wiggling the wire and
    // schedule an event that will be executed when the transmission is complete.
    //
    NS_ASSERT_MSG(m_txMachineState == READY, "Must be READY to transmit");
    // 必须在此处尝试捎带
    // 只有当 p 是数据包/重传包，且对应的 m_ack_flags 为 true 时才会生效
    PiggybackAck(p);
    piggycredit(p);
    m_txMachineState = BUSY;
    m_currentPkt = p;
    m_phyTxBeginTrace(m_currentPkt);
    Time txTime = Seconds(m_bps.CalculateTxTime(p->GetSize()));
    Time txCompleteTime = txTime + m_tInterframeGap;
    NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.GetSeconds() << "sec");
    Simulator::Schedule(txCompleteTime, &QbbNetDevice::TransmitComplete, this);

    bool result = m_channel->TransmitStart(p, this, txTime);
    if (result == false) {
        m_phyTxDropTrace(p);
    }
    return result;
}

Ptr<Channel> QbbNetDevice::GetChannel(void) const { return m_channel; }

bool QbbNetDevice::IsQbb(void) const { return true; }

void QbbNetDevice::NewQp(Ptr<RdmaQueuePair> qp) {
    qp->m_nextAvail = Simulator::Now();
    DequeueAndTransmit();
}
void QbbNetDevice::ReassignedQp(Ptr<RdmaQueuePair> qp) { DequeueAndTransmit(); }
void QbbNetDevice::TriggerTransmit(void) { DequeueAndTransmit(); }

void QbbNetDevice::SetQueue(Ptr<BEgressQueue> q) {
    NS_LOG_FUNCTION(this << q);
    m_queue = q;
}

Ptr<BEgressQueue> QbbNetDevice::GetQueue() { return m_queue; }

Ptr<RdmaEgressQueue> QbbNetDevice::GetRdmaQueue() { return m_rdmaEQ; }

void QbbNetDevice::RdmaEnqueueHighPrioQ(Ptr<Packet> p) {
    m_traceEnqueue(p, 0);
    m_rdmaEQ->EnqueueHighPrioQ(p);
}

void QbbNetDevice::TakeDown() {
    // TODO: delete packets in the queue, set link down
    if (m_node->GetNodeType() == 0) {
        // clean the high prio queue
        m_rdmaEQ->CleanHighPrio(m_traceDrop);
        // notify driver/RdmaHw that this link is down
        m_rdmaLinkDownCb(this);
    } else {  // switch
        // clean the queue
        for (uint32_t i = 0; i < qCnt; i++) m_paused[i] = false;
        while (1) {
            Ptr<Packet> p = m_queue->DequeueRR(m_paused);
            if (p == 0) break;
            m_traceDrop(p, m_queue->GetLastQueue());
        }
        // TODO: Notify switch that this link is down
    }
    m_linkUp = false;
}

void QbbNetDevice::UpdateNextAvail(Time t) {
    if (!m_nextSend.IsExpired() && t < m_nextSend.GetTs()) {
        Simulator::Cancel(m_nextSend);
        Time delta = t < Simulator::Now() ? Time(0) : t - Simulator::Now();
        m_nextSend = Simulator::Schedule(delta, &QbbNetDevice::DequeueAndTransmit, this);
    }
}
}  // namespace ns3
