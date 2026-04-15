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
* Author: Yibo Zhu <yibzh@microsoft.com>
*/
#ifndef QBB_NET_DEVICE_H
#define QBB_NET_DEVICE_H

#include "ns3/point-to-point-net-device.h"
#include "ns3/broadcom-node.h"
#include "ns3/qbb-channel.h"
//#include "ns3/fivetuple.h"
#include "ns3/event-id.h"
#include "ns3/broadcom-egress-queue.h"
#include "ns3/ipv4.h"
#include "ns3/ipv4-header.h"
#include "ns3/udp-header.h"
#include "ns3/rdma-queue-pair.h"
#include <vector>
#include <deque>
#include <queue>
#include<map>
#include <unordered_map>
#include <ns3/rdma.h>
#include "flitheader.h"
#include"sr-header.h"
#include"rx-buffer.h"
//#include "sr-replaybuffer.h"
#include"flow-stat-tag.h"
#define MAX_SN 65536//序号空间
#define m_rttEstimate 5000//这个是冷却时间,单位是ns
namespace ns3 {
// 前向声明 (避免循环 include)
class SwitchMmu;
class SwitchNode;
inline int SeqDist(uint16_t seq1, uint16_t seq2) {
    // 利用 int16_t 的溢出特性计算循环序列号距离
    return (int16_t)(seq1 - seq2); 
}
class RdmaEgressQueue : public Object{
public:
// 【新增】定义回调类型和成员
    typedef Callback<void, Ptr<RdmaQueuePair>> TxQpFinishCallback;
    TxQpFinishCallback m_txQpFinishCb;
	static const uint32_t qCnt = 8;
	static uint32_t ack_q_idx;
  uint32_t m_mtu=1392;
	int m_qlast;
	uint32_t m_rrlast;
	Ptr<DropTailQueue> m_ackQ; // highest priority queue
	Ptr<RdmaQueuePairGroup> m_qpGrp; // queue pairs
	std::unordered_map<int32_t, Time> current_pause_time;

	// callback for get next packet
	typedef Callback<Ptr<Packet>, Ptr<RdmaQueuePair> > RdmaGetNxtPkt;
	RdmaGetNxtPkt m_rdmaGetNxtPkt;

	static TypeId GetTypeId (void);
	RdmaEgressQueue();
	Ptr<Packet> DequeueQindex(int qIndex);
	int GetNextQindex();
	Time GetEarliestPacingTime() const { return m_earliestPacing; }  // 最早的QP pacing到期时间
	int GetLastQueue();
	uint32_t GetNBytes(uint32_t qIndex);
	uint32_t GetFlowCount(void);
	Ptr<RdmaQueuePair> GetQp(uint32_t i);
	
	void EnqueueHighPrioQ(Ptr<Packet> p);
	void CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb);

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaDequeue;

private:
	Time m_earliestPacing;  // GetNextQindex 中因 pacing 被跳过的 QP 的最早到期时间
};

// 极其轻量级的“户口本”，完美解耦物理数据
struct FlitMeta {
    uint16_t seqNum;       // 序列号 (统一用 uint16_t，和 m_next_seq_num 类型一致)
      int physicalIndex;     // 交换机侧：在 MMU 物理大池子里的下标 (端侧无意义，填 -1)
      Ptr<Packet> localCopy; // 端侧：本地存储的 flit 副本 (交换机侧为 null，用 physicalIndex 去 MMU 取)
      bool isAcked;          // 是否已经被位图 (Bitmap) 提前确认了？
      bool isRetransmitting; // 是否已经在重传队列里了？(防止重复入队)
      int retryCount;        // 重传次数
      Time sendTime;         // 发送时间戳 (用于超时检测)

      FlitMeta()
          : seqNum(0), physicalIndex(-1), localCopy(nullptr),
            isAcked(false), isRetransmitting(false),
            retryCount(0), sendTime(Seconds(0)) {}
};
/**
 * \class QbbNetDevice
 * \brief A Device for a IEEE 802.1Qbb Network Link.
 */
class QbbNetDevice : public PointToPointNetDevice
{
  friend class SwitchNode;
public:
 //变量区***********************************************************************************************************************
  static const uint32_t qCnt = 8;	// Number of queues/priorities used
  static const uint32_t pCnt = 128;	// Number of ports used
  static const uint32_t fCnt = 128; // Max number of flows on a NIC, for TX and RX respectively. TX+RX=fCnt*2
  static const uint32_t maxHop = 1; // Max hop count in the network. should not exceed 16 

 //作为向下游的发送端需要维护的变量
  std::queue<int> m_txQueue;           // 正常发送队列 (只存物理下标)
  std::deque<uint16_t> m_retransQueue;     // 紧急重传队列 (只存物理下标，优先级最高)
  uint16_t m_txTotalSent;       // 我累计发送了多少 Flit
  uint16_t m_txLimit;           // Switch 允许我发到的上限 (Flit)
  uint16_t m_next_seq_num;//下一个要分配的flit的序号
  bool nakflag;
  uint16_t nakseq;
  uint64_t nakbitmapHigh;  // 替换原来的 uint32_t nakbitmap
  uint64_t nakbitmapLow;
  bool creditflag;//表示现在是有待发送给接收端的信用更新的
  uint64_t m_rxCumulativeFreed;// 记录我这边累计接收/释放了多少，准备告诉对方
  enum ControlState {
    CTRL_NONE = 0, // 无事发生
    CTRL_ACK     // 有 ACK 需要捎带 (Lazy)
};

 //作为接收上游消息的接收端
 Ptr<RxBuffer> m_rxBuffer;//这个是重排序缓冲区，也是接收缓冲区
  std::queue<Ptr<Packet>> m_readyQueue;//这个队列里的包已经准备好要发了，等待被 TryForwardingRxBuffer() 转发到交换机里
  uint16_t m_last_acked_seq;//最后一个被接收端ack的flit序号，也就是发送窗口左边界的上一个序号
  uint16_t m_rxNext;  // 接收端期望的下一个序号 (即 expected_seq)
  
   uint32_t curpktpayload;//当前处理的包的总共payload大小
  // 状态数组：每个 VC 一个状态
  ControlState m_ctrl_state[8];
  // 序号数组：每个 VC 对应的 payload (即 expected_seq)
  uint16_t m_ctrl_seq[8];
    // 内部转发引擎状态
    bool m_rxStalled;                    // 我当前是否被大老板 (锁或 MMU) 卡住了？
    bool m_forwarding = false;           // 防重入：TryForwardingRxBuffer 正在执行中
    // 交换机侧需要的指针 (端侧不使用)
    Ptr<SwitchMmu> m_mmu;         // 交换机 MMU (内存管理)
    SwitchNode* m_switchNode;     // 所属交换机节点
    uint32_t m_portId;            // 本端口在交换机中的端口号

    // 核心重传账本：带状态的滑动窗口
    std::deque<FlitMeta> m_slidingWindow;//这个账本的作用是把已发送未确认的包都放进来
    // --- 新增状态变量 ---发送端要用的
    Ptr<Packet> m_currentLargePacket; // 当前正在被切片的大包
    uint32_t m_currentFlitIdx;        // 当前切到了第几个 Flit
    uint32_t m_currentleftsize;      // 当前大包还剩多少字节没切
    uint32_t m_totalFlits;            // 这个大包总共要切多少个 Flit
    uint32_t m_totalBytes;           // 这个大包的总字节数
    
    const uint32_t m_flitSize = 256;
    const uint16_t MAX_VALID_WINDOW = 32768;
    uint32_t m_bufferSize;    // 接收端重排序缓冲区容量(flit数), 必须等于初始credit
    uint32_t m_creditInit;    // 初始 credit (= m_bufferSize)
    Time m_rtoValue;          // RTO 超时值

    // PFC 模式状态
    bool m_pfcPauseSent;           // 是否已向上游发送过 PAUSE
    uint32_t m_pfcHighMark;        // RX buffer 高水位 (flit 数)
    uint32_t m_pfcLowMark;         // RX buffer 低水位 (flit 数)
    double m_pfcHighThreshold;     // 高水位占 m_bufferSize 比例
    double m_pfcLowThreshold;      // 低水位占 m_bufferSize 比例
    //sr要维护的变量，发送端
   //uint32_t m_txUna;//窗口左边缘，最早发出去但是还没收到确认的SX
   //Ptr<ReplayBuffer> m_replayBuffer;//重传缓冲区
   //std::deque<uint16_t> m_retransQueue; // 端侧待重传序号队列

 //函数区*************************************************************************************************************************
  void InitCredit(); // 在属性系统设置完成后调用，用 m_creditInit 初始化 m_bufferSize/m_txLimit/m_rxBuffer
  void TryForwardingRxBuffer(); // 尝试转发 RX Buffer 里的包（每次收到新包或被唤醒时调用）
  void TriggerCreditSendIfNeeded(); // 若有待发 credit 且 TX 空闲，立即触发发送（供 RequestForward 调用）
  void EnqueueTxIndex(int physicalIndex);// TX 端口调用的接口：把一个物理下标放进发送队列
  void PrintBitmap(uint64_t high, uint64_t low);
  // 职责：打上序号 + 存入重传缓冲区
    void PrepareNewPacket(Ptr<Packet> p);
    void TriggerAck(uint8_t vc_id, uint16_t seq);
    void TriggerNak(uint8_t vc_id, uint16_t seq);
    void PiggybackAck(Ptr<Packet> p);
    void generteStandaloneControl(); // 生成一个独立的控制包 (ACK 或 NAK)，不捎带在数据包上
    void SendNextFlit(); // 负责切一刀并发送，端侧用的
    void piggycredit(Ptr<Packet> p);//捎带credit的函数
    void ReleaseRxCredit (uint16_t flitsFreed);//device用来更新信用变量的函数
     void UpdateRtoTimer();
     void HandleRtoTimeout ();
     void HandleCumulativeACK(uint64_t ackSeq);  // 处理累计确认 (交换机侧: 清理 slidingWindow + 释放 MMU)
     void HandleBitmapNAK(uint64_t baseSeq, uint64_t bitmapLow, uint64_t bitmapHigh);  // 处理位图 NAK
     Ptr<Packet> GetFlitFromWindow(uint16_t seqNum);  // 辅助：从 slidingWindow 取 flit (自动分发到 MMU 或 localCopy)
     void FreeFlitInWindow(FlitMeta& meta);           // 辅助：释放一个 FlitMeta 的存储 (自动分发)
     // PFC 方法
     void CheckPfcThresholds();                          // 检查 RX buffer 水位，触发 PAUSE/RESUME
     void HandlePfcPause(uint32_t qIndex, uint32_t pauseTime);  // TX 侧处理收到的 PAUSE
     void HandlePfcResume(uint32_t qIndex);              // TX 侧处理收到的 RESUME
     void PfcResumeTimeout(uint32_t qIndex);             // PAUSE 安全超时回调
   //*********************************************************************************** */
  static TypeId GetTypeId (void);

  QbbNetDevice ();
  virtual ~QbbNetDevice ();

  /**
   * Receive a packet from a connected PointToPointChannel.
   *
   * This is to intercept the same call from the PointToPointNetDevice
   * so that the pause messages are honoured without letting
   * PointToPointNetDevice::Receive(p) know
   *
   * @see PointToPointNetDevice
   * @param p Ptr to the received packet.
   */
  virtual void Receive (Ptr<Packet> p);

  /**
   * Send a packet to the channel by putting it to the queue
   * of the corresponding priority class
   *
   * @param packet Ptr to the packet to send
   * @param dest Unused
   * @param protocolNumber Protocol used in packet
   */
  virtual bool Send(Ptr<Packet> packet, const Address &dest, uint16_t protocolNumber);
  //virtual bool SwitchSend (uint32_t qIndex, Ptr<Packet> packet,CustomHeader &ch);

  /**
   * Get the size of Tx buffer available in the device
   *
   * @return buffer available in bytes
   */
  //virtual uint32_t GetTxAvailable(unsigned) const;

  /**
   * TracedCallback hooks
   */
  void ConnectWithoutContext(const CallbackBase& callback);
  void DisconnectWithoutContext(const CallbackBase& callback);

  bool Attach (Ptr<QbbChannel> ch);

   virtual Ptr<Channel> GetChannel (void) const;

   //void SetQueue (Ptr<BEgressQueue> q);
   //Ptr<BEgressQueue> GetQueue ();
   virtual bool IsQbb(void) const;
   void NewQp(Ptr<RdmaQueuePair> qp);
   void ReassignedQp(Ptr<RdmaQueuePair> qp);
   void TriggerTransmit(void);

   bool IsQbbEnabled(void) { return m_qbbEnabled; }

   uint32_t SendPfc(uint32_t qIndex, uint32_t type); // type: 0 = pause, 1 = resume

   TracedCallback<Ptr<const Packet>, uint32_t> m_traceEnqueue;
   TracedCallback<Ptr<const Packet>, uint32_t> m_traceDequeue;
   TracedCallback<Ptr<const Packet>, uint32_t> m_traceDrop;
   TracedCallback<uint32_t> m_tracePfc; // 0: resume, 1: pause
 protected:

   //Ptr<Node> m_node;

   bool TransmitStart (Ptr<Packet> p);

   virtual void DoDispose(void);

   /// Reset the channel into READY state and try transmit again
   virtual void TransmitComplete(void);

   /// Look for an available packet and send it using TransmitStart(p)
   virtual void DequeueAndTransmit(void);

   /// Resume a paused queue and call DequeueAndTransmit()
   //virtual void Resume(unsigned qIndex);

   /**
   * The queues for each priority class.
   * @see class Queue
   * @see class InfiniteQueue
   */
   //Ptr<BEgressQueue> m_queue;

   Ptr<QbbChannel> m_channel;

   //pfc
   bool m_qbbEnabled;	//< PFC behaviour enabled
   bool m_qcnEnabled;
   bool m_dynamicth;
   uint32_t m_pausetime;	//< Time for each Pause
   bool m_paused[qCnt];	//< Whether a queue paused
   EventId m_resumeEvt[qCnt];

   //qcn

   /* RP parameters */
   EventId  m_nextSend;		//< The next send event
   EventId  m_rtoEvent;		//< RTO 超时重传定时器
   /* State variable for rate-limited queues */

   //qcn

   struct ECNAccount{
     Ipv4Address source;
     uint32_t qIndex;
     uint32_t port;
     uint8_t ecnbits;
     uint16_t qfb;
     uint16_t total;
  };

  std::vector<ECNAccount> *m_ecn_source;

public:
	Ptr<RdmaEgressQueue> m_rdmaEQ;
	void RdmaEnqueueHighPrioQ(Ptr<Packet> p);

	// callback for processing packet in RDMA
	typedef Callback<int, Ptr<Packet>, CustomHeader&,uint32_t> RdmaReceiveCb;
	RdmaReceiveCb m_rdmaReceiveCb;
	// callback for link down
	typedef Callback<void, Ptr<QbbNetDevice> > RdmaLinkDownCb;
	RdmaLinkDownCb m_rdmaLinkDownCb;
	// callback for sent a packet
	typedef Callback<void, Ptr<RdmaQueuePair>, Ptr<Packet>, Time> RdmaPktSent;
	RdmaPktSent m_rdmaPktSent;

	Ptr<RdmaEgressQueue> GetRdmaQueue();
	void TakeDown(); // take down this device
	void UpdateNextAvail(Time t);

	TracedCallback<Ptr<const Packet>, Ptr<RdmaQueuePair> > m_traceQpDequeue; // the trace for printing dequeue
};

} // namespace ns3

#endif // QBB_NET_DEVICE_H
