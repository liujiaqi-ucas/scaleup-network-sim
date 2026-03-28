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
                            MakeTraceSourceAccessor(&QbbNetDevice::m_tracePfc))
            .AddAttribute("CreditInit", "Initial credit and RxBuffer size in flits",
                          UintegerValue(256),
                          MakeUintegerAccessor(&QbbNetDevice::m_creditInit),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("RtoValue", "RTO timeout value",
                          TimeValue(MicroSeconds(20)),
                          MakeTimeAccessor(&QbbNetDevice::m_rtoValue),
                          MakeTimeChecker());

    return tid;
}

QbbNetDevice::QbbNetDevice() {
    // m_rtoValue 和 m_creditInit 由 AddAttribute 默认值设定，
    // 可通过 Config::SetDefault 或 SetAttribute 在运行时覆盖
    NS_LOG_FUNCTION(this);
    m_ecn_source = new std::vector<ECNAccount>;
    for(int i=0;i<8;i++){
        m_ctrl_state[i]=CTRL_NONE;
    }
    m_txTotalSent = 0;
    // m_creditInit 此时已被 ns-3 属性系统设好 (默认 256)
    m_bufferSize = 256;//rx的buffer大小，单位是flit
    m_rxCumulativeFreed = 0;//我这边累计接收/释放了多少，初始一个也没接收，所以是0
    creditflag=false;//表示现在没有待发送给接收端的信用更新的，因为初始的信用已经设置好了
    m_rdmaEQ = CreateObject<RdmaEgressQueue>();
    m_next_seq_num = 0;//下一个要分配的flit的序号，从0开始
    //m_last_acked_seq = 0;
    m_currentleftsize = 0;//端侧用的，当前正在切的包还剩多少字节没切了
    //m_totalFlits = 0;//好像没啥用
    m_currentFlitIdx = 0;//端侧用的，当前正在切的包已经切了多少个flit了
    nakflag = false;//表示现在没有nak要发
    nakseq = 0;//如果有nak要发，这个nak是针对哪个序号的
    nakbitmapHigh = 0;
    nakbitmapLow = 0;
    m_rxBuffer = Create<RxBuffer>(256);//初始化构造rxbuffer，容量等于初始信用（单位是flit）
    m_mmu = nullptr;//这里得看一下switchnode怎么赋值的
    m_switchNode = nullptr;//这里得看一下switchnode怎么赋值的
    m_portId = 0;
    m_rxStalled = false;//我当前是否被大老板 (锁或 MMU) 卡住了？初始没有，所以是 false
    //m_last_acked_seq = 0;
    m_rxNext = 0;
    m_txLimit = 256;//目前下游限制我发多少
}

QbbNetDevice::~QbbNetDevice() { NS_LOG_FUNCTION(this); }
void QbbNetDevice::UpdateRtoTimer() {
      // -------------------------------------------------------
      // 职责：确保 RTO 定时器对准窗口中"最老的、还需要关注的"未确认 flit
      // 调用时机：每次窗口状态变化后（发送新 flit、收到 ACK、重传发出后）
      // -------------------------------------------------------

      // 1. 窗口空了 → 没什么好监控的，关灯走人
      if (m_slidingWindow.empty()) {
          if (m_rtoEvent.IsRunning()) m_rtoEvent.Cancel();
          return;
      }

      // 2. 找最老的"值得关注"的未确认 flit
      //    跳过 isAcked（已确认）和 isRetransmitting（已入队待发，等发出去再监控）
      Time oldestSendTime;
      bool found = false;
      for (const auto& meta : m_slidingWindow) {
          if (!meta.isAcked && !meta.isRetransmitting) {
              oldestSendTime = meta.sendTime;
              found = true;
              break;  // deque 按发送顺序排列，第一个就是最老的
          }
      }

      // 3. 所有未确认的都已经在重传队列里了 → 等它们发出去后自然会重新 UpdateRtoTimer
      if (!found) {
          if (m_rtoEvent.IsRunning()) m_rtoEvent.Cancel();
          return;
      }

      // 4. 计算定时器应该在什么时刻触发（指数退避：用窗口头部的 retryCount）
      uint32_t headRetry = m_slidingWindow.front().retryCount;
      uint32_t backoffShift = (headRetry < 7) ? headRetry : 7; // 最多 2^7 = 128倍
      Time effectiveRto = Time(m_rtoValue.GetNanoSeconds() * (1u << backoffShift));
      Time deadline = oldestSendTime + effectiveRto;
      Time now = Simulator::Now();

      // 5. 取消旧定时器，设新的
      if (m_rtoEvent.IsRunning()) m_rtoEvent.Cancel();

      if (deadline <= now) {
          // 已经过期了！用 ScheduleNow 异步触发（避免同步递归栈爆炸）
          m_rtoEvent = Simulator::ScheduleNow(&QbbNetDevice::HandleRtoTimeout, this);
      } else {
          m_rtoEvent = Simulator::Schedule(deadline - now,
                                            &QbbNetDevice::HandleRtoTimeout, this);
      }
  }
  void QbbNetDevice::HandleRtoTimeout() {
      // -------------------------------------------------------
      // GBN RTO 策略：找到窗口 HEAD（最老未确认 seq），触发"回退N步"
      // 原因：GBN 接收端丢弃了所有乱序包，RTO 超时意味着 HEAD 及之后
      //       的包都需要重传，直接等价于调用 HandleGbnNAK(HEAD.seqNum)
      // 配合指数退避防止 RTO 风暴
      // -------------------------------------------------------
      Time now = Simulator::Now();
      bool anyRetrans = false;

      // 找窗口 HEAD（第一个未确认、未在重传中的 seq）
      uint16_t gbnBase = 0;
      bool foundHead = false;
      for (auto& meta : m_slidingWindow) {
          if (meta.isAcked) continue;
          if (meta.isRetransmitting) continue;

          uint32_t backoffShift = (meta.retryCount < 7) ? meta.retryCount : 7;
          Time effectiveRto = Time(m_rtoValue.GetNanoSeconds() * (1u << backoffShift));

          if ((now - meta.sendTime) >= effectiveRto) {
              gbnBase = meta.seqNum;
              foundHead = true;

              if (meta.retryCount <= 20 || meta.retryCount % 100 == 0) {
                  std::cout << "[RTO-GBN] Node=" << m_node->GetId()
                            << " Dev=" << m_ifIndex
                            << " HeadSeq=" << meta.seqNum
                            << " Retry=" << meta.retryCount
                            << " RTO=" << effectiveRto.GetMicroSeconds() << "us" << std::endl;
              }
          }
          break; // 只检查 HEAD
      }

      // GBN "go-back-N"：从 HEAD 开始将所有未确认包加入重传队列
      if (foundHead) {
          for (auto& meta : m_slidingWindow) {
              if (meta.isAcked) continue;
              if (meta.isRetransmitting) continue;
              int16_t dist = (int16_t)((uint16_t)meta.seqNum - gbnBase);
              if (dist >= 0) {
                  meta.isRetransmitting = true;
                  meta.retryCount++;
                  m_retransQueue.push_back(meta.seqNum);
                  anyRetrans = true;
              }
          }
      }

      // 重新设置定时器
      UpdateRtoTimer();

      if (anyRetrans && m_txMachineState == READY) {
          DequeueAndTransmit();
      }
  }
void QbbNetDevice::DoDispose() {
    NS_LOG_FUNCTION(this);
    
     if (m_rtoEvent.IsRunning()) {

     m_rtoEvent.Cancel();

    }
    PointToPointNetDevice::DoDispose();
}
// =========================================================
  // 辅助函数：从 slidingWindow 取 flit (自动分发到 MMU 或 localCopy)
  // =========================================================
  Ptr<Packet> QbbNetDevice::GetFlitFromWindow(uint16_t seqNum) {
      if (m_slidingWindow.empty()) return nullptr;

      uint16_t windowBase = m_slidingWindow.front().seqNum;
      int16_t dist = (int16_t)(seqNum - windowBase);
  if (dist < 0 || dist >= (int)m_slidingWindow.size()) return nullptr;


      auto& meta = m_slidingWindow[dist];
      if (m_mmu) {
          // 交换机侧：去 MMU 取
          return m_mmu->ReadFlit(meta.physicalIndex);
      } else {
          // 端侧：直接拿本地副本
          return meta.localCopy;
      }
  }

  // =========================================================
  // 辅助函数：释放一个 FlitMeta 的存储 (自动分发)
  // =========================================================
  void QbbNetDevice::FreeFlitInWindow(FlitMeta& meta) {
      if (meta.isAcked) return; // 已经释放过了

      meta.isAcked = true;
      if (m_mmu) {
          // 交换机侧：归还 MMU 物理空间
          m_mmu->FreeSpace(meta.physicalIndex, m_portId);
      } else {
          // 端侧：释放本地智能指针 (引用计数降为 0 时自动回收)
          meta.localCopy = nullptr;
      }
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
         //std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"因为有信用要释放要触发dequeue了"<<std::endl;
         DequeueAndTransmit();
     }
}
void QbbNetDevice::SendNextFlit() {  // 这个目前只是端侧的逻辑，
    //std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"执行了sendnextflit函数"<<std::endl;
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
        //std::cout << "[PKT_SEND] Node=" << nodeId
          //<< " Dev=" << devIdx
          //<< " Seq=" << ch.udp.seq
          //<< " Size=" << m_currentLargePacket->GetSize()
          //<< " Flit seq="<<seq
          //<<" 这是第"<<m_currentFlitIdx<<"个flit"
          //<< std::endl;
    } else if (isFirst) {
        flitType = 0; // HEAD (带 IP 头的)
        flitPayload->PeekHeader(ch);
        //std::cout << "[PKT_SEND] Node=" << nodeId
          //<< " Dev=" << devIdx
          //<< " Seq=" << ch.udp.seq
          //<< " Size=" << m_currentLargePacket->GetSize()
          //<< " Flit seq="<<seq
          //<<" 这是第"<<m_currentFlitIdx<<"个flit"
          //<< std::endl;
    } else if (isLast) {
        flitType = 2; // TAIL
        //std::cout << "[PKT_SEND] Node=" << nodeId
          //<< " Dev=" << devIdx
          //<< " Size=" << m_currentLargePacket->GetSize()
          //<< " Flit seq="<<seq
          //<<" 这是第"<<m_currentFlitIdx<<"个flit"
          //<< std::endl;
    } else {
        flitType = 1; // BODY
        //std::cout << "[PKT_SEND] Node=" << nodeId
          //<< " Dev=" << devIdx
          //<< " Size=" << m_currentLargePacket->GetSize()
          //<< " Flit seq="<<seq
          //<<" 这是第"<<m_currentFlitIdx<<"个flit"
          //<< std::endl;
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
    //std::cout<<"Node "<<m_node->GetId()<<"给第"<<seq<<"个flit加头，fh.SetPktTotalBytes = "<<payloadsize<<std::endl;
    // 把 FlitHeader 贴到切片前面
    flitPayload->AddHeader(fh);
    //这里加上common头部了，就不算进去整体长度了，就当标签了
    CommonHeader co;
    co.SetFlitType(FLIT_TYPE_DATA);//0表示数据flit
    flitPayload->AddHeader(co);//把common header加上去
    // 捎带 ACK 和 Credit（利用数据包顺路带回流控信息）
    PiggybackAck(flitPayload);
    piggycredit(flitPayload);
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
    
   // 存入统一的 slidingWindow (替代 ReplayBuffer)                                                                                                                         
      FlitMeta meta;                                                                                                                                                          
      meta.seqNum = m_next_seq_num;
      meta.physicalIndex = -1;  // 端侧不用 MMU                                                                                                                               
      meta.localCopy = flitPayload->Copy();    
      meta.isAcked = false;                                                                                                                                                   
      meta.isRetransmitting = false;
      meta.retryCount = 0;                                                                                                                                                    
      meta.sendTime = Simulator::Now();
      m_slidingWindow.push_back(meta);                                                                                                                                        
   
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
    // 每次发 ACK 都顺便刷新 credit，防止 credit 包被丢后发送端永久饥饿
    creditflag = true;
    //std::cout<<"Node "<<m_node->GetId()<<"的device"<<m_ifIndex<<"收到flit了，执行triggerack函数，此时ackseq是"<<seq<<std::endl;
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
    // GBN: 不需要位图，接收端只丢弃乱序包而不缓存，发送端收到NAK后回退整个窗口重传
    // SR位图改为全零，接收方只用firstMissing即可
    nakbitmapHigh = 0;
    nakbitmapLow  = 0;

    //std::cout << "Node " << m_node->GetId() << "的device" << m_ifIndex
              //<< "发现乱序了，执行triggernak函数，此时nakseq是" << seq;
    //PrintBitmap(nakbitmapHigh, nakbitmapLow);
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
        fh.SetCredit(0, m_rxCumulativeFreed+m_bufferSize);
        p->AddHeader(fh);
        creditflag = false;
    }

    // 2. 装回 CommonHeader
    p->AddHeader(common);
}
void QbbNetDevice::generteStandaloneControl() {
    Ptr<Packet> p = nullptr;
    bool need_ack_nak = (m_ctrl_state[0] == CTRL_ACK);
    // 如果有控制信息
        if (need_ack_nak) {
            //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"有控制信息要发"<<std::endl;
            //NS_LOG_LOGIC("Creating Standalone Control Packet for VC " << i);

            // 1. 造空包 (Payload Size = 0)
             p = Create<Packet>(0);

            // 2. 准备头部
            FlitHeader fh;
            fh.SetType(3);   // TYPE = 3 (SINGLE)，表示单帧控制包
            fh.SetVcId(0);   // 填入对应的 VC
            fh.SetSeqNum(0); // 控制包不消耗 GBN 序号，填 0
            
           
            fh.SetAck(m_ctrl_seq[0]);  // 标记为 ACK，填入确认序号
            NS_LOG_INFO("Sending Standalone ACK for VC " << 0 << " Seq " << m_ctrl_seq[0]);
            

            // 4. 填入 Credit 信息 (顺便捎带)
            // 即使是专门发 ACK 的包，也别忘了把最新的信用带上
            if (creditflag) {
                //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"同时有流控信息也要发"<<std::endl;
                fh.SetCredit(0, m_rxCumulativeFreed+m_bufferSize); // 填入你的接收端释放计数
                creditflag = false; // 既然发出去了，标志位清零
            }

            // 5. 封包
            p->AddHeader(fh);
            CommonHeader co;
            co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
            p->AddHeader(co);
            // 6. 清除状态
            // 这个 ACK/NAK 已经随包发出了，任务完成
            m_ctrl_state[0] = CTRL_NONE;

            // 7. 发射！
            // TransmitStart 会设置 BUSY，并安排发送完成事件
            TransmitStart(p);
            
            // 重要：一次回调只发一个包。
            // 物理层变 BUSY 了，必须 return。
            // 等发完这个包，TransmitComplete 会再次调用 DequeueAndTransmit 处理剩下的。
            return; 
        }
        // 补充情况：如果没有 ACK/NAK，但有 Credit 急需发送 (避免死锁)
    // 如果 creditflag 为 true，且上面循环没触发发送
    if (creditflag) {
        //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"目前只有信用需要单独发送，m_rxCumulativeFreed是   "<<m_rxCumulativeFreed<<std::endl;
        // 随便找一个 VC (通常是 0) 发送纯 Credit Update
        p = Create<Packet>(0);
        FlitHeader fh;
        fh.SetType(3);
        fh.SetVcId(0);
        fh.SetCredit(0, m_rxCumulativeFreed+m_bufferSize);
        p->AddHeader(fh);
        CommonHeader co;
        co.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
        p->AddHeader(co);
        creditflag = false;
        TransmitStart(p);
        return;
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
        //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"进入DequeueAndTransmit函数"<<std::endl;
        //先检查有没有nak要发，nak是优先级最高的
         if(nakflag){
            
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
        
        //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"检查完nak了，继续检查ack和credit"<<std::endl;
        // 再检查重传队列
    // 再检查重传队列 (统一逻辑：端侧和交换机侧共用)
      if (!m_retransQueue.empty()) {
          uint16_t sn = m_retransQueue.front();
          m_retransQueue.pop_front();

          // 在 slidingWindow 里查找这个 SN
          if (m_slidingWindow.empty()) {
              DequeueAndTransmit();
              return;
          }
          uint16_t windowBase = m_slidingWindow.front().seqNum;
          int16_t offset = (int16_t)(sn - windowBase);

          // Lazy Removal: 如果已经被 ACK 或不在窗口内，跳过
          if (offset < 0 || offset >= (int)m_slidingWindow.size()) {
              DequeueAndTransmit();
              return;
          }

          auto& meta = m_slidingWindow[offset];
          if (meta.isAcked) {
              meta.isRetransmitting = false;
              DequeueAndTransmit();
              return;
          }

          // 解锁 isRetransmitting，允许下次 NAK 再次入队
          meta.isRetransmitting = false;
          meta.sendTime = Simulator::Now();

          // 从统一接口取包
          Ptr<Packet> retransPkt = GetFlitFromWindow(sn);
          if (retransPkt) {
              TransmitStart(retransPkt->Copy());
              UpdateRtoTimer();  // ← 新增：sendTime 已更新，重新校准定时器
          }
          return;
      }

    // 再检查有没有没切完的包
   if (m_currentLargePacket != nullptr) {
    //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"当前有大包正在切片，继续切片"<<std::endl;
            SendNextFlit();  // 切下一刀并发出去
            return;          // 直接返回，不要去调度新包
    }else {
        //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"当前没有大包正在切片，去调度新包"<<std::endl;
            // A. 询问调度器：下一个轮到谁？
            // 注意：GetNextQindex 只是计算，不会把包取出来，也不会改变 QP 状态
            int qIndex = m_rdmaEQ->GetNextQindex();

            if (qIndex != -1024) {  // 有队列需要发送 (ACK队列 还有 普通数据队列)
                //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"调度器给了我一个需要发送的队列，队列索引是"<<qIndex<<std::endl;
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
               //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"包级流控检查，累计将要发送"<<m_txTotalSent<<"个flit"
                    //<<"   下游目前让我发 "<<m_txLimit<<"个flit"<<std::endl;
                // 2. 【关键】计算剩余信用 (利用无符号减法的回绕特性)
                int16_t available = (int16_t)(limit - sent);
                //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"包级流控检查，剩余信用是 "<<available<<"个flit"<<std::endl;
                if (available >= (int16_t)requiredFlits) {
                    // >>>>>> 钱够了！允许产生包 >>>>>>
                    //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"包级流控检查通过，信用够发这个包了，我要发了"<<std::endl;
                    // a. 真正产生包 (此时 QP 的 snd_nxt 才会增加)真增加了吗，这个得去看看
                    Ptr<Packet> p = m_rdmaEQ->DequeueQindex(qIndex);  // 调用这个函数才会更新qp轮询的那个值
                    CustomHeader qw(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
                    p->PeekHeader(qw);
                    curpktpayload = p->GetSize() - qw.GetSerializedSize();  // 记录当前包的有效负载
                    m_txTotalSent += requiredFlits;  // 预先记账
                    //std::cout<<" Node  "<<m_node->GetId()<<"的device "<<m_ifIndex<<"累计将要发送"<<m_txTotalSent<<"个flit"
                    //<<"   下游目前让我发 "<<m_txLimit<<"个flit"<<std::endl;
                    // c. 记录 Trace
                    m_traceQpDequeue(p, qp);

                    // d. 更新 Pacing (UpdateNextAvail)
                    // 告诉 QP 这个包发完了，下次什么时候能再发
                    m_rdmaPktSent(qp, p, m_tInterframeGap);//这个真的有用吗,现在除了这个其他的都能确认了
                         // e. 启动切片状态机
                        m_currentLargePacket = p;
                        //m_totalFlits = requiredFlits;  // 也可以用 p->GetSize() 再算一次更精确的
                        m_currentFlitIdx = 0;
                        m_currentleftsize = m_totalBytes;
                        SendNextFlit();
                        return;
                } else{
                    // >>>>>> 钱不够！不能发 >>>>>>>
                    //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"包级流控检查不通过，信用不够发这个包，我不能发了"<<std::endl;
                }
                
            }else{
                //代表没有东西要发送，那就得判断一下是不是又ack或者credit的需求了
                //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"调度器没有给我需要发送的队列了"<<std::endl;
            }
            //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"转发队列目前是空，我要看看有没有ack或者credit需求"<<std::endl;
         // 没包可发 (队列空 或 被流控阻塞)
        // 检查是否有欠下的 ACK 需要单独发送 (兜底机制)
        // 还得检查是不是有 credit 要发送
        generteStandaloneControl();
        //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<" standalone control检查完毕，退出DequeueAndTransmit函数"<<std::endl;
         return;
        
        }
    }  // =========================================================
       // 场景 2: Switch (交换机) - 【融合流控版】
       // =========================================================
    else {
        //第一优先级就是nak
        if(nakflag){
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
    //重传包已经是成品（有 SeqNum），不需要再 Check 信用
    // =================================================================
    // 第二优先级是重传包 (统一使用 m_retransQueue)
      while (!m_retransQueue.empty()) {
          uint16_t sn = m_retransQueue.front();
          m_retransQueue.pop_front();

          if (m_slidingWindow.empty()) continue;
          uint16_t windowBase = m_slidingWindow.front().seqNum;

          // 回绕安全检查：sn 是否已经被累计确认掉了
          int16_t dist = (int16_t)(sn - windowBase);
          if (dist < 0) {
              continue;  // 已被 ACK，历史幽灵包
          }

          int16_t offset = (int16_t)(sn - windowBase);
          if (offset >= 0 && offset < (int)m_slidingWindow.size()) {
              auto& meta = m_slidingWindow[offset];

              if (meta.isAcked) {
                  meta.isRetransmitting = false;
                  continue;
              }

              meta.isRetransmitting = false;
              meta.sendTime = Simulator::Now();

              // 统一取包接口
              Ptr<Packet> flit = GetFlitFromWindow(sn);
              if (flit) {
                  TransmitStart(flit->Copy());
                  UpdateRtoTimer();
                  return;
              }
          }
      }
    
        
    // =================================================================
    // 第三优先级是从转发队列取新包
    // =================================================================
    int16_t remaining = (int16_t)(m_txLimit - m_txTotalSent);
    if (!m_txQueue.empty() && remaining > 0) {// 先检查转发队列里有没有包，如果有包了再检查信用够不够
        m_txTotalSent += 1; // 先预占一个信用位，允许这个包发出去了，剩下的信用就少了一个了
        // 从正常队列里拿出来的，是纯粹的物理下标
        int physicalIndex = m_txQueue.front();
        m_txQueue.pop();

        // 去 MMU 提货
        Ptr<Packet> flit = m_mmu->ReadFlit(physicalIndex);

        CommonHeader common;
      common.SetFlitType(FLIT_TYPE_DATA);//0代表数据flit
      FlitHeader fh;
      flit->RemoveHeader(fh);           // 1. 取下旧头 (端侧的或上一跳的)
      fh.SetSeqNum(m_next_seq_num);  // 2. 更新为本端口的发送序号
      //m_next_seq_num++;              // 3. 指针自增
      flit->AddHeader(fh);              // 4. 装回新头
      flit->AddHeader(common);
      // 捎带 ACK 和 Credit（转发包顺路带回本端收到的流控信息）
      PiggybackAck(flit);
      piggycredit(flit);
      //做一些统计
      m_snifferTrace(flit);
      m_promiscSnifferTrace(flit);
      FlowIdTag t;
      //uint32_t qIndex = m_queue->GetLastQueue();    
      //m_node->SwitchNotifyDequeue(m_ifIndex, qIndex, p);
      flit->RemovePacketTag(t);
      //m_traceDequeue(p, qIndex);
        // 调用底层物理发送（必须用Copy()，避免Channel传给接收端的是同一个Ptr<Packet>对象，
      // 接收端RemoveHeader会破坏MMU里存的原始flit，导致重传时包头丢失）
        TransmitStart(flit->Copy());

        // 【状态转移】：发送完毕，绝不释放 MMU，而是将它转入重传账本！
        FlitMeta meta;
          meta.seqNum = m_next_seq_num;
          meta.physicalIndex = physicalIndex;                                                                                                                                 
          meta.localCopy = nullptr;  // 交换机侧不用本地副本
          meta.isAcked = false;                                                                                                                                               
          meta.isRetransmitting = false;                                                                                                                                      
          meta.retryCount = 0;          
          meta.sendTime = Simulator::Now();                                                                                                                                   
                                           
          m_slidingWindow.push_back(meta);
        m_next_seq_num++;
        UpdateRtoTimer();  // ← 新增
        return; // 物理发送完成，退出
    }
    generteStandaloneControl();//如果重传包和转发队列都没有数据的话，就单独检查一下有没有单独发送nak或者credie的需求
    return;
}
}

//**************************************************************************************** */
void QbbNetDevice::HandleCumulativeACK(uint64_t ackSeq) {
    bool spaceFreed = false;

      // 统一逻辑：只要窗口头部序号 < ackSeq，就弹出并释放
      while (!m_slidingWindow.empty()) {
          uint16_t headSeq = m_slidingWindow.front().seqNum;
          // 利用 uint16_t 回绕安全比较：headSeq 在 ackSeq "左边" 才弹出
          int16_t dist = (int16_t)(headSeq - ackSeq);
          if (dist >= 0) break;  // headSeq >= ackSeq，停止

          auto& head = m_slidingWindow.front();
          if (!head.isAcked) {
              FreeFlitInWindow(head);
              spaceFreed = true;
          }
          m_slidingWindow.pop_front();
      }

      // 交换机侧特有：释放空间后唤醒被阻塞的 RX 端口
      if (spaceFreed && m_switchNode) {
          m_switchNode->NotifySpaceAvailable();
      }
      UpdateRtoTimer();  // ← 新增：窗口头部移动了，重新对准定时器
}

// =========================================================
// 核心二：处理 128 位位图 NAK (Bitmap NAK)
// =========================================================
// =========================================================
// GBN (Go-Back-N) NAK 处理
// =========================================================
// 与 SR 的 HandleBitmapNAK 不同，GBN 不使用位图：
//   - 接收端丢弃所有乱序包，只接受按序到达的包
//   - 发送端收到 NAK(baseSeq) 后，必须从 baseSeq 开始"回退"，
//     将窗口中 baseSeq 及其后所有未确认包全部重传
void QbbNetDevice::HandleGbnNAK(uint64_t baseSeq) {
    // 1. 累积确认：baseSeq 之前的包已被接收端顺序接收
    HandleCumulativeACK(baseSeq);
    if (m_slidingWindow.empty()) return;

    // 2. GBN "go-back-N"：将 baseSeq 及之后所有未确认包加入重传队列
    //    这对应接收端丢弃了 baseSeq 以及后续所有乱序到达的包
    for (auto& meta : m_slidingWindow) {
        if (meta.isAcked) continue;
        if (meta.isRetransmitting) continue;
        // 检查 seq 是否在 baseSeq 及其后（用有符号距离避免回绕问题）
        int16_t dist = (int16_t)((uint16_t)meta.seqNum - (uint16_t)baseSeq);
        if (dist >= 0) {
            meta.isRetransmitting = true;
            meta.retryCount++;
            m_retransQueue.push_back(meta.seqNum);
        }
    }

    // 交换机侧：释放 MMU 空间让 ingress 继续
    if (m_switchNode) {
        m_switchNode->NotifySpaceAvailable();
    }
}

void QbbNetDevice::PrintBitmap(uint64_t high, uint64_t low) {
    //std::cout << "Bitmap High: " << std::bitset<64>(high)
              //<< " Low: "        << std::bitset<64>(low) << std::endl;
}

//**************************************************************************** */
void QbbNetDevice::EnqueueTxIndex(int physicalIndex) {
    // TX 端的账本更新！
    // 仅仅是把物理下标存入正常发送队列，O(1) 极速操作
    m_txQueue.push(physicalIndex);
    
    // NS_LOG_DEBUG("Port " << m_portId << " received new flit index " << physicalIndex << " to transmit.");
}
void QbbNetDevice::TryForwardingRxBuffer() {
    
    // 只要有准备好的 Flit，就一直尝试往交叉开关里塞
    while (!m_readyQueue.empty()) {
        
        Ptr<Packet> flit = m_readyQueue.front();

        // 直接向大老板申请转发 (SwitchNode 会在内部处理查锁、查 MMU、记录备忘录)
        ForwardStatus status = m_switchNode->RequestForward(m_portId, flit);

        if (status == FORWARD_SUCCESS) {
            // 转发成功！丢弃这个 Flit，看下一个
            m_readyQueue.pop();
            m_rxStalled = false;
            creditflag = true; // 转发成功了，说明对端已经有机会收到包了，可能会有 ACK/NAK 和 Credit 要发了
            m_rxCumulativeFreed++; // 这个是我接收端释放的计数，捎带更新一下

        } 
        else if (status == BLOCKED_BY_LOCK || status == BLOCKED_BY_MMU) {
            // 转发失败！被锁或者被 MMU 卡住了
            m_rxStalled = true;
            
            // 【绝对的原子性保护】：立刻 return！
            // 这个 Flit 依然稳稳地停在队头。
            // 当 SwitchNode 轮询仲裁点名到我时，我会用同一个 Flit 再次发起请求！
            return; 
        }
    }
}
void QbbNetDevice::Receive(Ptr<Packet> packet) {
    NS_LOG_FUNCTION(this << packet);
    if (!m_linkUp) {
        m_traceDrop(packet, 0);
        return;
    }

    if (m_receiveErrorModel && m_receiveErrorModel->IsCorrupt(packet)) {
        std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到的包发生了错误，直接丢弃"<<std::endl;
    CommonHeader co;
    packet->RemoveHeader(co);
    int cotype=co.GetFlitType();
        m_phyRxDropTrace(packet);
        return;
    }
    CommonHeader co;
    packet->RemoveHeader(co);
    int cotype=co.GetFlitType();
    if(cotype==1){//代表这是一个nak的flit
       
        NackHeader nak;
        packet->PeekHeader(nak);
        uint16_t firstMissing = nak.GetFirstMissing();
        // GBN: 只需要 firstMissing，bitmap 字段忽略
        // 收到 NAK 后触发"回退N步"：从 firstMissing 开始重传所有未确认包
        HandleGbnNAK(firstMissing);
        //std::cout<<"处理完nak之后的重传缓冲区状态"<<std::endl;
        //m_replayBuffer->PrintBuffer(m_node->GetId(), m_ifIndex);
        DequeueAndTransmit(); // 处理完 NAK 后，尝试发送重传包或新包
        return;
    }
    //std::cout<<"Node  "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到一个flit了，不是nak，准备处理这个flit"<<std::endl;
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
        //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到的包太小，无法包含flit头部，直接丢弃"<<std::endl;
        return;
    }
    // 3. 现在可以提取序号了
    uint16_t seq = fh.GetSeqNum();
    uint8_t vc = fh.GetVcId();
    uint8_t type = fh.GetType();
    uint32_t  payloadsize=fh.GetPktTotalBytes();
    //std::cout<<"Node "<<m_node->GetId()<<"收到的flit头部携带的pkttotalbytes大小是"<<payloadsize<<"  收到的seq是  "<<seq<<std::endl;
    // ---------------------------------------------------------
    // 第一步：无条件提取捎带信息 (Process Piggyback FIRST)
    // ---------------------------------------------------------
    // 无论这个 Flit 是数据还是控制，是乱序还是重复，
    // 它携带的 "对方对我的确认" 和 "对方给我的流控" 都是宝贵的。

    // 1. 处理对方捎带过来的 ACK/NACK
    if (fh.HasAck()) {  // 如果有捎带ack/nak的话
        //std::cout<<"Node "<<m_node->GetId()<<"收到了ack，序号是"<<fh.GetAckSeq()<<std::endl;
        //ProcessAck(fh.GetAckSeq());
        HandleCumulativeACK(fh.GetAckSeq());
        fh.setackflag(0);//收到捎带之后需要把这个捎带标记清除，要不然可能会让下游产生误解
    }

    if (fh.HasCredit()) {
        fh.setcreditflag(0);
        uint16_t currentCredit = fh.GetCreditLimit();
        m_txLimit = currentCredit;
        //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"收到了下游的流控回复   "<<"现在的信用限制是"<<m_txLimit<<std::endl;
        
        //std::cout<<"Node "<<m_node->GetId()<<" device  "<<m_ifIndex<<"因为收到流控要触发dequeue了"<<std::endl;
        if (m_txMachineState == READY) {
            DequeueAndTransmit();  // 收到新信用后立刻重试，防止信用到达时设备空转
        }
        
    }

    // ---------------------------------------------------------
    // 第二步：区分 Flit 类型
    // ---------------------------------------------------------
    if (packetsize == fh.GetSerializedSize()) {
        // 这是一个纯控制包（比如专门发的 NACK 或 Credit Update）
        // 收到对端的 ACK/Credit 时，顺便刷新本端的 credit，防止 credit 包丢失后上游永久饥饿
        creditflag = true;
        DequeueAndTransmit(); // 处理完控制信息后，尝试发送重传包或新包
        return;
    }
    
    // -------------------------------------------------------------
    // 2. 区分节点类型：交换机 vs 网卡
    // -------------------------------------------------------------
    
    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    // 分支 A: 交换机逻辑 (Switch)
    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    if (m_node->GetNodeType() > 0) {
        // =====================================================
        // GBN 交换机接收逻辑：只接受按序到达的包
        // 乱序包直接丢弃并发 NAK（不缓存，不等待填坑）
        // =====================================================

        uint16_t dist = (uint16_t)(seq - m_rxNext);

        // 1. 过期包（已确认的重传）
        if (dist >= MAX_SN / 2) {
            TriggerAck(0, m_rxNext);
            DequeueAndTransmit();
            return;
        }

        // 2. GBN 核心：只接受 seq == m_rxNext（按序）
        if (dist == 0) {
            // 按序到达：加头后直接入转发队列
            packet->AddHeader(fh);
            m_readyQueue.push(packet);
            m_rxNext = (m_rxNext + 1) % MAX_SN;
            TriggerAck(0, m_rxNext);
            TryForwardingRxBuffer();
        } else {
            // 乱序到达：GBN 直接丢弃，发 NAK 要求从 m_rxNext 重传
            // 注意：GBN 不缓存乱序包，发送端收到 NAK 后回退整个窗口重传
            TriggerNak(0, m_rxNext);
        }

        DequeueAndTransmit();
        return; // 交换机逻辑结束
    }

    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    // 分支 B: 网卡逻辑 (NIC / End-Host)
    // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
    else {
        // =====================================================
        // GBN 网卡接收逻辑：只接受按序到达的包
        // 乱序包直接丢弃并发 NAK，不缓存，等发送端回退重传
        // =====================================================

        uint16_t dist = (seq - m_rxNext + MAX_SN) % MAX_SN;

        // 1. 过期包（已确认过的重传）
        if (dist >= MAX_SN / 2) {
            TriggerAck(0, m_rxNext);
            return;
        }

        // 2. GBN 核心：只接受 seq == m_rxNext（按序到达）
        if (dist == 0) {
            // 按序到达：加头后直接提交给 RDMA 层
            packet->AddHeader(fh);
            m_rdmaReceiveCb(packet, ch, m_ifIndex);
            m_rxNext = (m_rxNext + 1) % MAX_SN;

            // 反馈：ACK 和释放一个 Credit（GBN 每次只提交一包）
            TriggerAck(0, m_rxNext);
            ReleaseRxCredit(1);
        } else {
            // 乱序到达：GBN 直接丢弃，发 NAK
            // 发送端收到 NAK 后将从 m_rxNext 开始"回退N步"重传
            TriggerNak(0, m_rxNext);
        }
    }
    return;
}
bool QbbNetDevice::Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber) {
    NS_ASSERT_MSG(false, "QbbNetDevice::Send not implemented yet\n");
    return false;
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
        PiggybackAck(p);
        piggycredit(p);
    }else{
        //std::cout<<"Node "<<m_node->GetId()<<" device "<<m_ifIndex<<"发的包是一个nak包，不能捎带ack和credit"<<std::endl;
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

// SetQueue / GetQueue removed — BEgressQueue 已不再使用

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
        // BEgressQueue 已移除，交换机侧清理由 MMU 管理
        for (uint32_t i = 0; i < qCnt; i++) m_paused[i] = false;
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







     
