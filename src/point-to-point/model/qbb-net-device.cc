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
#include <bitset> // 必须包含这个头文件
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
                // 【核心调用】这会直接触发 RdmaHw::DeleteQueuePair(qp)
                 m_txQpFinishCb(qp);
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
    // 假设 RTT 是 2.5us，RTO 设为 50us 比较稳妥
    m_rtoBase = MicroSeconds(50); 
    m_rtoValue = m_rtoBase;

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
    //m_replay_trigger_seq = 0;
    m_currentleftsize = 0;
    m_totalFlits = 0;
    m_currentFlitIdx = 0;
    //sr的参数初始化
    m_txUna = 0;
    m_rxNext = 0;
    m_forwardNext = 0;
    nakflag = false;
    nakseq = 0;
    //nakbitmap = 0;
    nakbitmapHigh = 0;
    nakbitmapLow = 0;
    m_nackCooldown = MicroSeconds(5); // 默认 200 微秒冷却,这个是需要计算的参数
    m_lastNackTime = Seconds(0);
    m_replayBuffer = Create<ReplayBuffer>(m_bufferSize);
    m_rxBuffer = Create<RxBuffer>(m_bufferSize);
    // 【新增】初始化为 0 或一个不可能的值
    m_lastNakSeq = 0;
}

QbbNetDevice::~QbbNetDevice() { NS_LOG_FUNCTION(this); }

void QbbNetDevice::DoDispose() {
    NS_LOG_FUNCTION(this);
    
     if (m_rtoEvent.IsRunning()) {

     m_rtoEvent.Cancel();

    }
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
 void QbbNetDevice::UpdateRtoTimer() {
     // 1. 如果窗口已经空了 (所有包都确认了)，取消定时器
     if (m_txUna == m_next_seq_num) {
         if (m_rtoEvent.IsRunning()) {
             m_rtoEvent.Cancel();
         }
         m_rtoValue = m_rtoBase; // 恢复初始 RTO
         return;
     }

     // 2. 如果窗口不空，且定时器没在跑，说明我们需要为当前的 m_txUna 启动计时
    if (m_rtoEvent.IsRunning()) {
        m_rtoEvent.Cancel();
    }
    m_rtoEvent = Simulator::Schedule(m_rtoValue, &QbbNetDevice::HandleRtoTimeout, this);
 }
 void QbbNetDevice::HandleRtoTimeout() {
     NS_LOG_WARN("RTO Timeout occurred for SN: " << m_txUna << " at " << Simulator::Now());

     // 1. 强制重传 m_txUna (最老的未确认包)
     // 检查它是否已经在重传队列里，避免重复入队
     if (!m_replayBuffer->IsRetransmitting(m_txUna)) {
         m_retransQueue.push_back(m_txUna);
         std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据rto要重传了，targetSn是"<<m_txUna<<std::endl;
         m_replayBuffer->SetRetransmitting(m_txUna, true);
        
         NS_LOG_INFO("RTO: Pushed " << m_txUna << " to retrans queue");
     }

     // 2. 指数退避 (Exponential Backoff)
     // 防止网络严重拥塞时，重传导致拥塞加剧。下次超时时间翻倍。
     m_rtoValue += m_rtoValue;
    
     // 3. 立即尝试发送
     // 这一步很重要！必须去唤醒发送引擎消费刚才加入队列的包
    
         DequeueAndTransmit(); 
    

     // 4. 重启定时器
     // 只要窗口不空，就得一直盯着，直到收到 ACK 为止
     m_rtoEvent = Simulator::Schedule(m_rtoValue, &QbbNetDevice::HandleRtoTimeout, this);
 }
//
// bool QbbNetDevice::CheckNackCooldown() {
//     // 获取当前仿真时间
//     Time now = Simulator::Now();

//     // 计算距离上次发送过了多久
//     if (now - m_lastNackTime > m_nackCooldown) {
//         return true; // 已经冷静下来了，可以发
//     } else {
//         return false; // 还在冷却中，憋着别发
//     }
// }
// 【修改】增加参数 currentMissingSeq
bool QbbNetDevice::CheckNackCooldown(uint16_t currentMissingSeq) {
    Time now = Simulator::Now();

    // ---------------------------------------------------------
    // 逻辑 1: 新故障豁免 (New Hole Bypass)
    // ---------------------------------------------------------
    // 如果这次丢的包 (currentMissingSeq) 和上次报错的包 (m_lastNakSeq) 不一样，
    // 说明这是链路上的一个新坑！
    // 这种情况不需要受冷却时间限制，必须立刻报错，否则吞吐量会掉。
    if (currentMissingSeq != m_lastNakSeq) {
        m_lastNakSeq = currentMissingSeq; // 记住这次的新坑
        return true; // 【豁免】立刻允许发送
    }

    // ---------------------------------------------------------
    // 逻辑 2: 常规冷却 (Cooldown)
    // ---------------------------------------------------------
    // 如果 currentMissingSeq == m_lastNakSeq，说明还是同一个包没收到。
    // 这时候必须检查时间，防止 NAK 风暴。
    if (now - m_lastNackTime > m_nackCooldown) {
        return true; // 冷却结束，允许重发
    } 
    
    // 既不是新坑，也没过冷却期 -> 憋着
    return false; 
}
void QbbNetDevice::ReleaseRxCredit(uint16_t flitsFreed) {
    //std::cout<<"Node "<<m_node->GetId()<<"  device "<<m_ifIndex<<"执行了releaserxcredit函数，m_rxCumulativeFreed目前是"<<m_rxCumulativeFreed<<std::endl;
    Ptr<SwitchNode> swNode = DynamicCast<SwitchNode>(m_node);
    m_rxCumulativeFreed += flitsFreed;
    creditflag = true;  // 告诉device有新的credit要发送
    // if(m_node->GetNodeType() == 1){
    // std::cout<<"Node "<<m_node->GetId()<<"  device "<<m_ifIndex<<"执行了releaserxcredit函数，m_rxCumulativeFreed目前是"<<m_rxCumulativeFreed
    // <<"目前我的ingress还有"<<swNode->m_mmu->m_ingressQueues[m_ifIndex].size()<<"个flit等待转发"
    // <<std::endl;
    // }else{
    //     std::cout<<"Node "<<m_node->GetId()<<"  device "<<m_ifIndex<<"执行了releaserxcredit函数，m_rxCumulativeFreed目前是"<<m_rxCumulativeFreed
    // <<std::endl;
    // }
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
    //m_next_seq_num++; 

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
    //这里加上common头部了，就不算进去整体长度了，就当标签了
    CommonHeader co;
    co.SetFlitType(FLIT_TYPE_DATA);//0表示数据flit
    flitPayload->AddHeader(co);//把common header加上去
    // =========================================================
    // 【关键新增】 搬运身份证 (Tag)
    // =========================================================
    FlowStatTag fst;
    if (m_currentLargePacket->PeekPacketTag(fst)) {
        // 复制 FlowStatTag (包含时间戳)
        flitPayload->AddPacketTag(fst);
    }
    
    FlowIDNUMTag fint;
    if (m_currentLargePacket->PeekPacketTag(fint)) {
        // 复制 FlowIDNUMTag (包含流 ID 和总大小)
        flitPayload->AddPacketTag(fint);
    }
    // =========================================================
    // 6. 【存入重传缓冲区】
    // =========================================================
    
   // std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<":  "<<std::endl;
    m_replayBuffer->AddNewPacket(m_next_seq_num,flitPayload->Copy());
    std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"把seq为"<<m_next_seq_num<<"的flit存入重传缓冲区了"<<std::endl;
    //m_replayBuffer->PrintBuffer(m_node->GetId(), m_ifIndex);
    m_next_seq_num++;
   

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
    // // 【植入点 2】
    // // 每次发完包，都喊一声：“保姆，去看看定时器开了没？没开就开一下。”
     UpdateRtoTimer();
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
    // 发现乱序，标记为 NAK,这里把nak标志都设完了，然后就在transmitstart还有dequeue那里分辨了
    // nakflag = true;
    // nakseq = seq;
    // nakbitmap=m_rxBuffer->GenerateNackBitmap(nakseq);
    // // NAK 很急！即使没有数据要发，也得赶紧造一个控制包发出去
    // std::cout<<"Node "<<m_node->GetId()<<"的device"<<m_ifIndex<<"发现乱序了，执行triggernak函数，此时nakseq是"<<seq;
    // PrintBitmap(nakbitmap);
    // if (m_txMachineState == READY) {
        
    //     std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<" 因为有nak要发所以要dequeue了 "<<std::endl;
    //     DequeueAndTransmit(); 
    // }
    nakflag = true;
    nakseq = seq;
    // 改为生成128位位图
    m_rxBuffer->GenerateNackBitmap(nakseq, nakbitmapHigh, nakbitmapLow);

    std::cout << "Node " << m_node->GetId() << "的device" << m_ifIndex
              << "发现乱序了，执行triggernak函数，此时nakseq是" << seq;
    PrintBitmap(nakbitmapHigh, nakbitmapLow);
    if (m_txMachineState == READY) {
        DequeueAndTransmit();
    }
}
// 这个函数负责把 ACK "缝"到数据包上。它严格遵守 "同 VC 捎带" 原则
void QbbNetDevice::PiggybackAck(Ptr<Packet> p) {
    // 1. 先剥离最外层的 CommonHeader
    CommonHeader common;
    p->RemoveHeader(common); 

    // 2. 现在最外层是 FlitHeader 了，可以安全操作
    FlitHeader fh;
    p->PeekHeader(fh);
    
    uint8_t current_vc = fh.GetVcId();
    if (current_vc >= 8) {
        // 记得把 CommonHeader 加回去！
        p->AddHeader(common); 
        return;
    }

    if (m_ctrl_state[current_vc] != CTRL_NONE) {
        p->RemoveHeader(fh); // 取下 FlitHeader
        
        if (m_ctrl_state[current_vc] == CTRL_ACK) {
            fh.SetAck(m_ctrl_seq[current_vc]);
        }
        
        p->AddHeader(fh);    // 装回 FlitHeader
        m_ctrl_state[current_vc] = CTRL_NONE;
    }

    // 3. 最后把 CommonHeader 装回去
    p->AddHeader(common); 
}
void QbbNetDevice::piggycredit(Ptr<Packet> p) {
    // 1. 剥离 CommonHeader
    CommonHeader common;
    p->RemoveHeader(common);

    FlitHeader fh;
    p->PeekHeader(fh);
    uint8_t current_vc = fh.GetVcId();

    if (current_vc >= 8) {
        p->AddHeader(common);
        return;
    }

    if (creditflag) {
        p->RemoveHeader(fh);
        fh.SetCredit(0, m_rxCumulativeFreed);
        p->AddHeader(fh);
        creditflag = false;
    }

    // 2. 装回 CommonHeader
    p->AddHeader(common);
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
        //先检查有没有nak要发，nak是优先级最高的
         if(nakflag){
            //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"有nak要发了，nakseq是"<<nakseq<<"nakbitmap是"<<nakbitmap<<std::endl;
            // Ptr<Packet> p = Create<Packet>(0);
            //  NackHeader nh;
            // nh.SetFirstMissing(nakseq);
            // nh.SetBitmap(nakbitmap);
            // p->AddHeader(nh);
            // CommonHeader co;
            // co.SetFlitType(FLIT_TYPE_NACK);//1表示控制flit
            // p->AddHeader(co);
            // nakflag=false;
            // TransmitStart(p);
            // return;
            Ptr<Packet> p = Create<Packet>(0);
            NackHeader nh;
            nh.SetFirstMissing(nakseq);
            nh.SetBitmap(nakbitmapHigh, nakbitmapLow); // 改为128位
            p->AddHeader(nh);
            CommonHeader co;
            co.SetFlitType(FLIT_TYPE_NACK);
            p->AddHeader(co);
            nakflag = false;
            TransmitStart(p);
            return;
        }
        // 再检查有没有没切完的包
        // if (m_currentLargePacket != nullptr) {
        //     SendNextFlit();  // 切下一刀并发出去
        //     return;          // 直接返回，不要去调度新包
        // }
        
        // 再检查重传队列
    if (!m_retransQueue.empty()) {
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"重传队列有包要发"<<std::endl;
        // 1. 【取号】：获取待重传的 SN
    uint16_t sn = m_retransQueue.front();
    m_retransQueue.pop_front(); // 拿到号就立刻从队列移除
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"从重传队列取出了SN="<<sn<<std::endl;

    // 2. 【验号】：Lazy Removal (懒惰删除) 检查
    // 如果在排队期间，这个包已经被 ACK 确认了，就不需要重传了
    if (m_replayBuffer->IsAcked(sn)) {
        //std::cerr << "!!! DEBUG: SN=" << sn << " 竟然被判定为 ACKED ??? 跳过重传 !!!" << std::endl;
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"重传队列的SN="<<sn<<"已经被ACK了，跳过重传"<<std::endl;
        NS_LOG_INFO("Skipping retrans for ACKed SN: " << sn);
        
        // 顺手解锁（虽然它已经Acked了，但保持状态一致性是好习惯）
        m_replayBuffer->SetRetransmitting(sn, false);
        
        // 跳过本次发送，或者递归调用 TrySend() 处理下一个
        DequeueAndTransmit();
        return; 
    }

    // 3. 【解锁】：解除 "Retransmitting" 锁定状态
    // 这是你之前最担心的问题。在这里解锁，表示包已经离开队列，
    // 如果下次再丢，允许它再次进入重传队列。
    m_replayBuffer->SetRetransmitting(sn, false); 

    // 4. 【刷新】：更新上次发送时间 (防止 NACK 风暴)
    m_replayBuffer->UpdateLastSentTime(sn);

    // 5. 【提货】：去仓库 (ReplayBuffer) 拿数据
    // 注意：GetFlit 返回的是 Ptr<Flit>
     p = m_replayBuffer->GetFlit(sn);
     CommonHeader cotemp;
     FlitHeader fhtemp;
     p->RemoveHeader(cotemp);
     p->PeekHeader(fhtemp);
     std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"从重传缓冲区拿出了SN="<<sn<<"，这个包的SeqNum是"<<fhtemp.GetSeqNum()<<std::endl;
     p->AddHeader(cotemp);
    // 防御性检查：万一指针是空的（极少见）
    if (p) {
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据重传队列拿出的包的SN="<<sn<<"准备发出去了"<<std::endl;
        NS_LOG_INFO("Switch Priority Sending Retransmission SN=" << sn);
        // 6. 【发货】：调用底层的发送函数
        NS_LOG_INFO("Switch Resending UID=" << p->GetUid());
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据重传队列拿出的包的SN="<<sn<<"准备发出去了"<<std::endl;
        TransmitStart(p->Copy()); // 
    }else{
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据重传队列拿出的包竟然是空指针"<<std::endl;
    }
    return;
    } 
    // 再检查有没有没切完的包
   if (m_currentLargePacket != nullptr) {
            SendNextFlit();  // 切下一刀并发出去
            return;          // 直接返回，不要去调度新包
    }else {
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
        
        // 遍历 8 个 VC，防止跨 VC 阻塞
        for (uint32_t i = 0; i < 8; i++) {
        
        // 检查两个条件：
        // 1. 是否有 ACK/NAK 要发 (m_ctrl_state)
        // 2. 是否有 Credit 要发 (creditflag) —— 即使没有 ACK，光发 Credit 也是有意义的
        
        bool need_ack_nak = (m_ctrl_state[i] == CTRL_ACK);
        
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
            
           
            fh.SetAck(m_ctrl_seq[i]);  // 标记为 ACK，填入确认序号
            NS_LOG_INFO("Sending Standalone ACK for VC " << i << " Seq " << m_ctrl_seq[i]);
            

            // 4. 填入 Credit 信息 (顺便捎带)
            // 即使是专门发 ACK 的包，也别忘了把最新的信用带上
            if (creditflag) {
                std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"同时有流控信息也要发"<<std::endl;
                fh.SetCredit(0, m_rxCumulativeFreed); // 填入你的接收端释放计数
                creditflag = false; // 既然发出去了，标志位清零
            }

            // 5. 封包
            p->AddHeader(fh);
            CommonHeader co;
            co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
            p->AddHeader(co);
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
        CommonHeader co;
        co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
        p->AddHeader(co);
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
        //第一优先级就是nak
        if(nakflag){
            //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"有nak要发了，nakseq是"<<nakseq<<"nakbitmap是"<<nakbitmap<<std::endl;
            // Ptr<Packet> p = Create<Packet>(0);
            //  NackHeader nh;
            // nh.SetFirstMissing(nakseq);
            // nh.SetBitmap(nakbitmap);
            // p->AddHeader(nh);
            // CommonHeader co;
            // co.SetFlitType(FLIT_TYPE_NACK);//1表示控制flit
            // p->AddHeader(co);
            // std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"nak包封包完毕了"<<std::endl;
            // nakflag=false;
            // TransmitStart(p);
            // return;
            Ptr<Packet> p = Create<Packet>(0);
            NackHeader nh;
            nh.SetFirstMissing(nakseq);
            nh.SetBitmap(nakbitmapHigh, nakbitmapLow); // 改为128位
            p->AddHeader(nh);
            CommonHeader co;
            co.SetFlitType(FLIT_TYPE_NACK);
            p->AddHeader(co);
            nakflag = false;
            TransmitStart(p);
            return;
        }
    // =================================================================
    //第二优先级是重传包
    //重传包已经是成品（有 SeqNum），不需要再 Check 信用（假设享有特权）
    // =================================================================
    if (!m_retransQueue.empty()) {
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"重传队列有包要发,重传队列的长度是"<<m_retransQueue.size()<<std::endl;
        // 1. 【取号】：获取待重传的 SN
    uint16_t sn = m_retransQueue.front();
    m_retransQueue.pop_front(); // 拿到号就立刻从队列移除
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"从重传队列取出了SN="<<sn<<std::endl;

    // 2. 【验号】：Lazy Removal (懒惰删除) 检查
    // 如果在排队期间，这个包已经被 ACK 确认了，就不需要重传了
    if (m_replayBuffer->IsAcked(sn)) {
        NS_LOG_INFO("Skipping retrans for ACKed SN: " << sn);
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"重传队列的SN="<<sn<<"已经被ACK了，跳过重传"<<std::endl;
        // 顺手解锁（虽然它已经Acked了，但保持状态一致性是好习惯）
        m_replayBuffer->SetRetransmitting(sn, false);
        
        // 跳过本次发送，或者递归调用 TrySend() 处理下一个
        DequeueAndTransmit();
        return; 
    }else{
         std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"重传队列的SN="<<sn<<"还没有被ACK"<<std::endl;
    }

    // 3. 【解锁】：解除 "Retransmitting" 锁定状态
    // 这是你之前最担心的问题。在这里解锁，表示包已经离开队列，
    // 如果下次再丢，允许它再次进入重传队列。
    m_replayBuffer->SetRetransmitting(sn, false); 

    // 4. 【刷新】：更新上次发送时间 (防止 NACK 风暴)
    m_replayBuffer->UpdateLastSentTime(sn);

    // 5. 【提货】：去仓库 (ReplayBuffer) 拿数据
    // 注意：GetFlit 返回的是 Ptr<Flit>
     p = m_replayBuffer->GetFlit(sn);
     CommonHeader cotemp;
     FlitHeader fhtemp;
     p->RemoveHeader(cotemp);
     p->PeekHeader(fhtemp);
     std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"从重传缓冲区拿出了SN="<<sn<<"，这个包的SeqNum是"<<fhtemp.GetSeqNum()<<std::endl;
     p->AddHeader(cotemp);
    
    // 防御性检查：万一指针是空的（极少见）
    if (p) {
        NS_LOG_INFO("Switch Priority Sending Retransmission SN=" << sn);
        // 6. 【发货】：调用底层的发送函数
        NS_LOG_INFO("Switch Resending UID=" << p->GetUid());
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据重传队列拿出的包的SN="<<sn<<"准备发出去了"<<std::endl;
        TransmitStart(p->Copy()); //
    }else{
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据重传队列拿出的包竟然是空指针"<<std::endl;
    }
    return;
    }
    //第三优先级是暂存包
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
            //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"转发队列目前是空，我要看看有没有ack或者credit需求"<<std::endl;
         // 没包可发 (队列空 或 被流控阻塞)
        // 检查是否有欠下的 ACK 需要单独发送 (兜底机制)
        // 还得检查是不是有 credit 要发送
       
        // 遍历 8 个 VC，防止跨 VC 阻塞
        for (uint32_t i = 0; i < 8; i++) {
        
        // 检查两个条件：
        // 1. 是否有 ACK/NAK 要发 (m_ctrl_state)
        // 2. 是否有 Credit 要发 (creditflag) —— 即使没有 ACK，光发 Credit 也是有意义的
        
        bool need_ack_nak = (m_ctrl_state[i] == CTRL_ACK);
        
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
            
           
            fh.SetAck(m_ctrl_seq[i]);  // 标记为 ACK，填入确认序号
            NS_LOG_INFO("Sending Standalone ACK for VC " << i << " Seq " << m_ctrl_seq[i]);
            

            // 4. 填入 Credit 信息 (顺便捎带)
            // 即使是专门发 ACK 的包，也别忘了把最新的信用带上
            if (creditflag) {
                std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"同时有流控信息也要发"<<std::endl;
                fh.SetCredit(0, m_rxCumulativeFreed); // 填入你的接收端释放计数
                creditflag = false; // 既然发出去了，标志位清零
            }

            // 5. 封包
            p->AddHeader(fh);
            CommonHeader co;
            co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
            p->AddHeader(co);
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
        CommonHeader co;
        co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
        p->AddHeader(co);
        creditflag = false;
        TransmitStart(p);
        return;
    }
            return; 
        }
    }
    // =================================================================
    // 第四优先级是从转发队列取新包
    // =================================================================
    if (m_queue->IsEmpty()) {//说明现在转发队列没有包，但是我要检查有没有需要ack或者credit的需求，我要单独造一个包
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"转发队列目前是空，我要看看有没有ack或者credit需求"<<std::endl;
         // 没包可发 (队列空 或 被流控阻塞)
        // 检查是否有欠下的 ACK 需要单独发送 (兜底机制)
        // 还得检查是不是有 credit 要发送
       
        // 遍历 8 个 VC，防止跨 VC 阻塞
        for (uint32_t i = 0; i < 8; i++) {
        
        // 检查两个条件：
        // 1. 是否有 ACK/NAK 要发 (m_ctrl_state)
        // 2. 是否有 Credit 要发 (creditflag) —— 即使没有 ACK，光发 Credit 也是有意义的
        
        bool need_ack_nak = (m_ctrl_state[i] == CTRL_ACK);
        
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
            
           
            fh.SetAck(m_ctrl_seq[i]);  // 标记为 ACK，填入确认序号
            NS_LOG_INFO("Sending Standalone ACK for VC " << i << " Seq " << m_ctrl_seq[i]);
            

            // 4. 填入 Credit 信息 (顺便捎带)
            // 即使是专门发 ACK 的包，也别忘了把最新的信用带上
            if (creditflag) {
                std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"同时有流控信息也要发"<<std::endl;
                fh.SetCredit(0, m_rxCumulativeFreed); // 填入你的接收端释放计数
                creditflag = false; // 既然发出去了，标志位清零
            }

            // 5. 封包
            p->AddHeader(fh);
            CommonHeader co;
            co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
            p->AddHeader(co);
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
        CommonHeader co;
        co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
        p->AddHeader(co);
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
    // CommonHeader common;
    // p->RemoveHeader(common);
    CommonHeader common;
    common.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
    FlitHeader fh;
    p->RemoveHeader(fh);           // 1. 取下旧头 (端侧的或上一跳的)
    fh.SetSeqNum(m_next_seq_num);  // 2. 更新为本端口的发送序号
    //m_next_seq_num++;              // 3. 指针自增
    p->AddHeader(fh);              // 4. 装回新头
    p->AddHeader(common);
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
    // 1. 计算物理索引 (找坑位)
   //    利用环形特性找到 m_next_seq_num 对应的数组下标
    //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<":  "<<std::endl;
    m_replayBuffer->AddNewPacket(m_next_seq_num,p->Copy());
    m_next_seq_num++;
    // 4.3 通知上层 (如果你的 SwitchNode 需要统计或处理)
     Ptr<SwitchNode> swNode = DynamicCast<SwitchNode>(m_node);
     if(swNode){
    swNode->releasecredit(m_ifIndex);//这里是向ingress释放一个空间
     }
    // 4.4 发射
    std::cout<<"switch Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"发新包了，序号是"<<fh.GetSeqNum()<<std::endl;
    TransmitStart(p);
    UpdateRtoTimer();

        //能走到这里的都是没有包可以发送的情况
    }
    

    
        
}

//**************************************************************************************** */
void QbbNetDevice::ProcessAck(uint16_t ackSn) {
    //NS_LOG_FUNCTION(this << ack_seq);
   // ==========================================
    // 1. 合法性判断 (Sanity Check)
    // ==========================================
    
    // 计算 ackSn 相对于 m_txUna 的距离
    // 假设 MAX_SN = 65536 (16 位序号空间)
    int dist = (ackSn - m_txUna + MAX_SN) % MAX_SN;

    // 条件 A: 重复/过期 ACK，ack代表的是ack之前的我都收到了
    // 如果距离为 0，说明对方确认的位置就是我现在的位置，无事发生
    if (dist == 0) {
        return; 
    }

    // 条件 B: 越界 ACK (确认了还没发的包)
    // 计算当前飞行中的窗口大小
    int flightSize = (m_next_seq_num - m_txUna + MAX_SN) % MAX_SN;
    if (dist > flightSize) {
        std::cout<<"Received ACK for unsent data! AckSn:" << ackSn 
                     << " TxNext:" << m_next_seq_num<<std::endl;
        return; // 严重错误，直接返回
    }

    // ==========================================
    // 2. 循环滑动窗口 (Slide Window)
    // ==========================================
    
    // 我们需要逐个处理从 m_txUna 到 ackSn 之间的每一个槽位
    // 为什么不直接跳？因为要清理 ReplayBuffer 的状态
    std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"现在收到ack现在开始清理槽位了 "<<std::endl;
    m_replayBuffer->FreeSlots(m_txUna, ackSn);
    std::cout<<"清理之后的重传缓冲区打印一下"<<std::endl;
    //m_replayBuffer->PrintBuffer(m_node->GetId(), m_ifIndex);
    // ==========================================
    // 3. 更新全局状态
    // ==========================================
    
    // 更新窗口左边缘
    std::cout<<"Window Slide: " << m_txUna << " -> " << ackSn<<std::endl;
    m_txUna = ackSn;
    // // ==========================================
    // // RTO 植入点 1：重置定时器
    // // ==========================================
    
    // // A. 只要收到了新 ACK，说明网络是通的，重置退避系数
     m_rtoValue = m_rtoBase; 

    // // B. 取消旧定时器 (因为之前的 m_txUna 已经搞定了)
     if (m_rtoEvent.IsRunning()) {
         m_rtoEvent.Cancel();
     }

    // // C. 如果还有未确认的包，UpdateRtoTimer 会自动启动新的计时
     UpdateRtoTimer();
    
}
void QbbNetDevice::PrintBitmap(uint64_t high, uint64_t low) {
    std::cout << "Bitmap High: " << std::bitset<64>(high)
              << " Low: "        << std::bitset<64>(low) << std::endl;
}
void QbbNetDevice::processnak(uint16_t firstMissing, uint64_t bitmapHigh, uint64_t bitmapLow) {
    //NS_LOG_FUNCTION(this << firstMissing << bitmap);

    // =========================================================
    // 第一步：处理隐式确认 (Implicit Cumulative ACK)
    // =========================================================
    // NACK 的含义是："firstMissing 没收到，但它之前的肯定都收到了"
    // 所以，我们可以先调用处理 ACK 的逻辑，把窗口推进到 firstMissing
    // (复用之前写的 ProcessCumulativeAck 函数)
    ProcessAck(firstMissing);
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据nak隐式确认了，firstMissing是"<<firstMissing<<std::endl;
    // =========================================================
    // 第二步：处理基准包 (Base Missing Packet)
    // =========================================================
    // firstMissing 这个包肯定是丢了，否则接收端不会报它
    
    // 检查这个包是否在重传缓冲区里（可能已经被隐式确认滑出去了，防越界）
    // 注意：利用 uint16_t 减法计算距离，判断是否在发送窗口内
    uint16_t dist = firstMissing - m_txUna;
    
    // 只有当它是合法的未确认包时才处理
    if (dist < m_bufferSize) { 
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"nak报告的firstMissing是合法的"<<std::endl;
        // 1. 检查是否已经确认 (可能 ACK 乱序先到了)
        bool alreadyAcked = m_replayBuffer->IsAcked(firstMissing);
        
        // 2. 检查是否正在重传 (防止重复入队)
        bool alreadyQueued = m_replayBuffer->IsRetransmitting(firstMissing);
        
        // 3. 检查冷却时间 (Cool-down / RTT Check)
        //    防止刚发出去的包因为 NACK 风暴被重复加队
        Time lastSent = m_replayBuffer->GetLastSentTime(firstMissing);
        bool isCoolingDown = (Simulator::Now() - lastSent) < NanoSeconds(m_rttEstimate);
        // 【关键修改】获取该包的重传次数
        uint8_t retxCount = m_replayBuffer->GetRetxCount(firstMissing);
        if (!alreadyAcked && !alreadyQueued && (!isCoolingDown || retxCount == 0)) {
            // ---> 加入高优先级队列
            m_retransQueue.push_back(firstMissing);
            std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"是"<<firstMissing<<std::endl;
            // ---> 上锁，标记为正在处理
            m_replayBuffer->SetRetransmitting(firstMissing, true);
            
            NS_LOG_INFO("Retransmit Base: " << firstMissing);
        }else{
            std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"不满足重传条件，targetSn是"<<firstMissing<<std::endl;
            std::cout<<"alreadyAcked "<<alreadyAcked<<" alreadyQueued "<<alreadyQueued<<" isCoolingDown "<<isCoolingDown<<std::endl;
        }
    }

    // =========================================================
    // 第三步：解析位图 (Parse Bitmap)
    // =========================================================
    // Bitmap 的 Bit 0 对应 firstMissing + 1
    // Bitmap 的 Bit 31 对应 firstMissing + 32
    
    for (int i = 0; i < 128; i++) {
        // uint16_t targetSn = firstMissing + 1 + i; // 自然回绕 (uint16_t)

        // // 【关键边界检查】
        // // 如果 targetSn 碰到了 m_next_seq_num，说明这个包我还没发呢！
        // // 接收端填 0 是因为它当然没收到。直接退出循环。
        // if (targetSn == m_next_seq_num) {
        //     break; 
        // }

        // // 读取位图的第 i 位
        // // 1 = 接收端已收到; 0 = 接收端未收到
        // bool peerHasIt = (bitmap >> i) & 1;

        // if (peerHasIt) {
        //     // --- Case A: 对方说收到了 (Bit = 1) ---
        //     // 标记为 ACK。如果它在重传队列里，Send的时候会自动忽略
        //     m_replayBuffer->MarkAcked(targetSn);//标记为已经被ack了，这里还没判断是不是正在处理
            
        // } else {
        //     // --- Case B: 对方说没收到 (Bit = 0) ---
        //     // 这时候不能无脑重传，必须过三关：
            
        //     bool isAcked = m_replayBuffer->IsAcked(targetSn);//是不是已经被ack了
        //     bool isQueued = m_replayBuffer->IsRetransmitting(targetSn);//是不是已经在重传队列中
            
        //     Time lastSent = m_replayBuffer->GetLastSentTime(targetSn);//冷却限制
        //     // 如果刚发出去不到 1 个 RTT，那这个 0 很正常（还在路上）
        //     bool isCoolingDown = (Simulator::Now() - lastSent) < NanoSeconds(m_rttEstimate);

        //     if (!isAcked && !isQueued && !isCoolingDown) {
        //         // ---> 确认为丢失，加入队列
        //         m_retransQueue.push_back(targetSn);
        //         std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"根据nak要重传了，targetSn是"<<targetSn<<std::endl;
        //         m_replayBuffer->SetRetransmitting(targetSn, true);
                
        //         NS_LOG_INFO("Retransmit Bitmap item: " << targetSn);
        //     }
        // }
        uint16_t targetSn = (uint16_t)(firstMissing + 1 + i);

        if (targetSn == m_next_seq_num) break;

        // 从 low/high 中取第 i 位
        bool peerHasIt;
        if (i < 64)
            peerHasIt = (bitmapLow  >> i) & 1ULL;
        else
            peerHasIt = (bitmapHigh >> (i - 64)) & 1ULL;

        if (peerHasIt) {
            m_replayBuffer->MarkAcked(targetSn);
        } else {
            bool isAcked       = m_replayBuffer->IsAcked(targetSn);
            bool isQueued      = m_replayBuffer->IsRetransmitting(targetSn);
            Time lastSent      = m_replayBuffer->GetLastSentTime(targetSn);
            bool isCoolingDown = (Simulator::Now() - lastSent) < NanoSeconds(m_rttEstimate);

            if (!isAcked && !isQueued && !isCoolingDown) {
                m_retransQueue.push_back(targetSn);
                m_replayBuffer->SetRetransmitting(targetSn, true);
            }
        }
    }
    DequeueAndTransmit();//这里因为加了一些重传包，直接触发一下，看看能不能重传
    
}
//**************************************************************************** */

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
    CommonHeader co;
    packet->RemoveHeader(co);
    int cotype=co.GetFlitType();
        m_phyRxDropTrace(packet);
        if(cotype==0){
        std::cout << "Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<" 丢了一个不是nak包 "<<std::endl;  // 丢弃包的时候打印一下日志 
                  
        }else{
        std::cout << "Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"丢了一个nak包了"<<std::endl;  // 丢弃包的时候打印一下日志
        }
        return;
    }
    CommonHeader co;
    packet->RemoveHeader(co);
    int cotype=co.GetFlitType();
    if(cotype==1){//代表这是一个nak的flit
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到一个nak包了，下面打印没处理之前的重传缓冲区状态"<<std::endl;
        //m_replayBuffer->PrintBuffer(m_node->GetId(), m_ifIndex);
        // NackHeader nak;
        // packet->PeekHeader(nak);
        // uint16_t firstMissing=nak.GetFirstMissing();
        // uint32_t bitmap=nak.GetBitmap();
        // std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到nak了，firstMissing是"<<firstMissing;
        // PrintBitmap(bitmap);
        // processnak(firstMissing, bitmap);
        // return;
        NackHeader nak;
        packet->PeekHeader(nak);
        uint16_t firstMissing = nak.GetFirstMissing();
        uint64_t bitmapHigh   = nak.GetBitmapHigh(); // 改为128位
        uint64_t bitmapLow    = nak.GetBitmapLow();
        std::cout << "Node  " << m_node->GetId() << " device " << m_ifIndex
                  << "收到nak了，firstMissing是" << firstMissing;
        PrintBitmap(bitmapHigh, bitmapLow);
        processnak(firstMissing, bitmapHigh, bitmapLow);
        
        std::cout<<"处理完nak之后的重传缓冲区状态"<<std::endl;
        //m_replayBuffer->PrintBuffer(m_node->GetId(), m_ifIndex);
        return;
    }
    std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到一个flit了，不是nak，准备处理这个flit"<<std::endl;
    //下面就代表的是一个数据flit，可能有流控和ack捎带
    int packetsize=packet->GetSize();
    m_macRxTrace(packet);
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    // 1. 准备一个空的 Header 对象（容器）
    FlitHeader fh;

    // 2. 【关键步骤】偷看头部 (Peek)
    // ns-3 会读取 packet 前 16 字节，填充到 fh 对象里
    // 注意：此时 p 内部的数据并没有被移除，p 还是完整的
    uint32_t bytes_read = packet->RemoveHeader(fh);//注意，这里我把flit的头部先给移除，为了改几个字段
    // 校验：确保真的读到了头 (防御性编程)
    if (bytes_read < fh.GetSerializedSize()) {
        NS_LOG_ERROR("Packet too small to contain FlitHeader!");
        std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到的包太小，无法包含flit头部，直接丢弃"<<std::endl;
        return;
    }
    // 3. 现在可以提取序号了
    uint16_t seq = fh.GetSeqNum();
    uint8_t vc = fh.GetVcId();
    uint8_t type = fh.GetType();
    uint32_t  payloadsize=fh.GetPktTotalBytes();
    std::cout<<"Node "<<m_node->GetId()<<"收到的flit头部携带的pkttotalbytes大小是"<<payloadsize<<"  收到的seq是  "<<seq<<std::endl;
    // ---------------------------------------------------------
    // 第一步：无条件提取捎带信息 (Process Piggyback FIRST)
    // ---------------------------------------------------------
    // 无论这个 Flit 是数据还是控制，是乱序还是重复，
    // 它携带的 "对方对我的确认" 和 "对方给我的流控" 都是宝贵的。

    // 1. 处理对方捎带过来的 ACK/NACK
    if (fh.HasAck()) {  // 如果有捎带ack/nak的话
        std::cout<<"Node "<<m_node->GetId()<<"收到了ack，序号是"<<fh.GetAckSeq()<<std::endl;
        ProcessAck(fh.GetAckSeq());
        fh.setackflag(0);//收到捎带之后需要把这个捎带标记清除，要不然可能会让下游产生误解
    }

    if (fh.HasCredit()) {
        fh.setcreditflag(0);
        uint16_t currentCredit = fh.GetCreditLimit();
        m_txLimit = currentCredit;
        std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到了下游的流控回复   "<<"现在的信用限制是"<<m_txLimit<<std::endl;
        
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
    
    // -------------------------------------------------------------
    // 2. 区分节点类型：交换机 vs 网卡
    // -------------------------------------------------------------
    
    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    // 分支 A: 交换机逻辑 (Switch)
    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    if (m_node->GetNodeType() > 0) { 
        
        // 1. 安检 (Validity Check)
        uint16_t dist = (seq - m_rxNext + MAX_SN) % MAX_SN;
        std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到的包的seq是"<<seq<<"，目前期待的seq是"<<m_rxNext<<"，距离是"<<dist<<std::endl;
        if (dist >= MAX_SN / 2) {
            std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到过期包了，seq是"<<seq<<std::endl;
            TriggerAck(0, m_rxNext);
            return;} // 过期
        if (dist >= m_bufferSize) 
        {
            std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到超出窗口范围的包了，seq是"<<seq<<std::endl;
            return; // 溢出
            }
        if (m_rxBuffer->IsReceived(seq)) 
        {   std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到重复包了，seq是"<<seq<<std::endl;
            return; // 重复
            }


        // 2. 入库 (Store)
        // 交换机后续转发需要完整的包，所以先把头加回去
        packet->AddHeader(fh);
        m_rxBuffer->StorePacket(seq, packet);
        // 在 Receive 的 StorePacket 前后：
        std::cout << "Receive: m_rxBuffer地址=" << m_rxBuffer << std::endl;
        m_rxBuffer->PrintDebugState(); // 打印 Buffer 状态，看看坑位和包的关系
        // 3. 状态更新与触发
        if (seq == m_rxNext) {
            // [填坑成功]
            
            // (1) 推进接收指针 (RxNext) 以便发送正确的 ACK
            // 注意：这里只推进 m_rxNext 表示"我收到了"，但【不清理 Buffer】(ClearEntry)
            // 因为交换机的转发逻辑还在后面，它需要从 Buffer 里取数据。
            // 交换机应该有另一个指针 (比如 m_forwardNext) 来负责取数据和清理。
            while (m_rxBuffer->IsReceived(m_rxNext)) {
                m_rxNext = (m_rxNext + 1) % MAX_SN;
            }
            std::cout<<"交换机Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到了期望的flit，填坑成功，seq是"<<seq<<"目前的m_rxNext是"<<m_rxNext<<"，现在开始转发了"<<std::endl;
            // (2) 触发转发 (Trigger)
            // 你说"只要触发一次转发，后面连续转发会自动进行"
            // 所以这里我们将当前包传给 SwitchReceiveFromDevice 作为触发信号
            // 或者这只是告诉交换机 "Port X 有新数据了"
            packet->AddPacketTag(FlowIdTag(m_ifIndex));
            CustomHeader dummyCh; 
            m_node->SwitchReceiveFromDevice(this, packet, dummyCh);//这里可能得检查一下会不会卡死回不来

            // (3) 发送 ACK (告诉上游我收齐到了哪里)
            TriggerAck(0, m_rxNext);

        } else {
            std::cout<<"交换机Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到了一个乱序的flit，seq是"<<seq<<"，期望的seq是"<<m_rxNext<<"，我先把它存起来，等它变成期望的seq的时候再转发"<<std::endl;
            // [乱序到达]
            // 只存包，发 NACK，不触发转发
            //if (CheckNackCooldown(m_rxNext)) {
                TriggerNak(0, m_rxNext);
                //m_lastNackTime = Simulator::Now();//这个变量是干嘛的呢
            //}
        }
        
        return; // 交换机逻辑结束
    }

    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    // 分支 B: 网卡逻辑 (NIC / End-Host)
    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    else {
        
        // 1. 安检 (Validity Check)
        // 1. 安检 (Validity Check)
        uint16_t dist = (seq - m_rxNext + MAX_SN) % MAX_SN;
        std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到的包的seq是"<<seq<<"，目前期待的seq是"<<m_rxNext<<"，距离是"<<dist<<std::endl;
        if (dist >= MAX_SN / 2) {
            std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到过期包了，seq是"<<seq<<std::endl;
            TriggerAck(0, m_rxNext);
            return;} // 过期
        if (dist >= m_bufferSize) 
        {
            std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到超出窗口范围的包了，seq是"<<seq<<std::endl;
            return; // 溢出
            }
        if (m_rxBuffer->IsReceived(seq)) 
        {   std::cout<<"端侧Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到重复包了，seq是"<<seq<<std::endl;
            return; // 重复
            }

        // 2. 入库 (Store)
        packet->AddHeader(fh); // 先加头
        m_rxBuffer->StorePacket(seq, packet);
        //m_rxBuffer->PrintDebugState(); // 打印 Buffer 状态，看看坑位和包的关系
        // 3. 提交循环 (Commit Loop)
        // 只有填坑成功才执行
        if (seq == m_rxNext) {
            std::cout<<"端侧Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到了期望的flit，填坑成功，seq是"<<seq<<"，现在开始提交了"<<std::endl;
            int commitCount = 0;

            // 循环处理 buffer 中所有连续的包
            while (m_rxBuffer->IsReceived(m_rxNext)) {
                Ptr<Packet> p = m_rxBuffer->GetPacket(m_rxNext);
                m_rdmaReceiveCb(p, ch,m_ifIndex);//把包交给上层，注意这里的回调函数是用户自己注册的，所以我不知道它会不会卡死回不来，如果卡死了那就说明上层处理不过来了，可能需要改成异步的回调机制了

                // d. 【关键】清理 Buffer & 推进指针
                // 网卡侧已经交付给上层了，必须立刻清理内存并释放空间
                //m_rxBuffer->ClearEntry(m_rxNext);
                m_rxBuffer->CommitHead();
                m_rxNext = (m_rxNext + 1) % MAX_SN;
                
                commitCount++;
            }
            std::cout<<"端侧Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"向上层传递完数据之后，现在要打印rxbuffer状态了"<<std::endl;
            //m_rxBuffer->PrintDebugState(); // 再次打印 Buffer 状态，看看提交后的变化
            // 4. 批量反馈 (Feedback)
            if (commitCount > 0) {
                // 发送 ACK
                TriggerAck(0, m_rxNext);
                
                // 批量释放 Credit (因为我们刚刚 ClearEntry 了 commitCount 个包)
                // 
                
                    ReleaseRxCredit(commitCount); // 替换为你实际的发送 Credit 函数
                
            }

        } else {
            std::cout<<"端侧Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到了一个乱序的flit，seq是"<<seq<<"，期望的seq是"<<m_rxNext<<"，我先把它存起来，等它变成期望的seq的时候再提交"<<std::endl;
            // [乱序到达]
            // 只存包，发 NACK，不提交
            //if (CheckNackCooldown(m_rxNext)) {
                TriggerNak(0, m_rxNext);
                //m_lastNackTime = Simulator::Now();
            //}
        }
    }
    return;
}
bool QbbNetDevice::cantransmit(){
    return m_forwardNext != m_rxNext;//这个条件为真代表可以转发，为假的话代表目前丢包不能转发
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
    CommonHeader co;
    p->PeekHeader(co);
    if (co.GetFlitType() == 0) { // 数据flit才能捎带
        std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"发的包是一个数据包，可以捎带ack和credit"<<std::endl;
        PiggybackAck(p);
        piggycredit(p);
    }else{
        std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"发的包是一个nak包，不能捎带ack和credit"<<std::endl;
    }
    //要是单nak的话直接发就行，不用捎带什么的
    m_txMachineState = BUSY;
    m_currentPkt = p;
    m_phyTxBeginTrace(m_currentPkt);
    Time txTime = Seconds(m_bps.CalculateTxTime(p->GetSize()));
    Time txCompleteTime = txTime + m_tInterframeGap;
    NS_LOG_LOGIC("Schedule TransmitCompleteEvent in " << txCompleteTime.GetSeconds() << "sec");
    Simulator::Schedule(txCompleteTime, &QbbNetDevice::TransmitComplete, this);
    // // 【植入点 2】
    // // 每次发完包，都喊一声：“保姆，去看看定时器开了没？没开就开一下。”
    // UpdateRtoTimer();
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
