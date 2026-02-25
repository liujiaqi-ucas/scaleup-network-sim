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
#include<map>
#include <unordered_map>
#include <ns3/rdma.h>
#include "flitheader.h"
#include"sr-header.h"
#include"rx-buffer.h"
#include "sr-replaybuffer.h"
#include"flow-stat-tag.h"
#define MAX_SN 65536//序号空间
#define m_bufferSize 40000//重传缓冲区的空间
#define m_rttEstimate 5000//这个是冷却时间,单位是ns
namespace ns3 {
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
	int GetLastQueue();
	uint32_t GetNBytes(uint32_t qIndex);
	uint32_t GetFlowCount(void);
	Ptr<RdmaQueuePair> GetQp(uint32_t i);
	
	void EnqueueHighPrioQ(Ptr<Packet> p);
	void CleanHighPrio(TracedCallback<Ptr<const Packet>, uint32_t> dropCb);

	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaEnqueue;
	TracedCallback<Ptr<const Packet>, uint32_t> m_traceRdmaDequeue;
};

/**
 * \class QbbNetDevice
 * \brief A Device for a IEEE 802.1Qbb Network Link.
 */
class QbbNetDevice : public PointToPointNetDevice 
{
public:
// 供 SwitchNode 调用的接口 (发送 Credit 包)
    void SendCreditPacket(uint32_t qIndex, uint16_t creditFlits);
  static const uint32_t qCnt = 8;	// Number of queues/priorities used
  static const uint32_t pCnt = 128;	// Number of ports used
  static const uint32_t fCnt = 128; // Max number of flows on a NIC, for TX and RX respectively. TX+RX=fCnt*2
  static const uint32_t maxHop = 1; // Max hop count in the network. should not exceed 16 
  //*******************************发送端要维护的变量**********************************************/
  void ReloadRetransQueue(uint16_t start_seq);// 辅助函数：清空并重新填装重传队列
  void PrintBitmap(uint64_t high, uint64_t low);
  // 职责：打上序号 + 存入重传缓冲区
    void PrepareNewPacket(Ptr<Packet> p);
    // 参数 is_nack: true 发 NACK, false 发 ACK (一般用于重复包的立即重确认)
// 参数 seq: 期望收到的序号 (对于 NACK) 或 确认序号 (对于 ACK)
void SendControlFlit(bool is_nack, uint16_t seq);
//void processnak(uint16_t firstMissing, uint32_t bitmap);//处理收到的nak的函数
void processnak(uint16_t firstMissing, uint64_t bitmapHigh, uint64_t bitmapLow); // 改为128位
void ProcessAck(uint16_t ack_seq);//处理收到的ack的函数
 //uint16_t m_next_seq_num;//下一个要分配的flit的序号
 uint16_t m_last_acked_seq;//最后一个被接收端ack的flit序号，也就是发送窗口左边界的上一个序号
 //Ptr<Packet> sendptr;//指向下一个待发送flit的指针
 //uint16_t m_replay_trigger_seq;//触发这一轮重传的序号，用于过滤重复的nak
 //ReplayBuffer m_replayBuffer; //重传缓冲区，存放发送完的flit
 // 我们的重传队列
  std::deque<uint16_t> m_retransQueue;
  //****************************************************************************** */
  //接收端需要维护的变量
  uint16_t expected_seq;//接收端期待收到的下一个flit的序号
  //****************************************************************************** */
  //捎带相关的变量和函数
  // ==========================================
    // 【新增】捎带确认的状态变量 (Array for Multi-VC)
    // ==========================================
    // 8个 VC 独立维护，互不干扰
    //bool m_ack_flags[8];          
    //uint16_t m_pending_acks[8];  

    enum ControlState {
    CTRL_NONE = 0, // 无事发生
    CTRL_ACK,      // 有 ACK 需要捎带 (Lazy)
    CTRL_NAK       // 有 NAK 需要发送 (Urgent)
};
   uint32_t curpktpayload;//当前处理的包的总共payload大小
  // 状态数组：每个 VC 一个状态
  ControlState m_ctrl_state[8];
  // 序号数组：每个 VC 对应的 payload (即 expected_seq)
  uint16_t m_ctrl_seq[8];
    
    // ==========================================
    // 【新增】辅助函数
    // ==========================================
    // 1. 收到包时调用，更新状态
    void TriggerAck(uint8_t vc_id, uint16_t seq);
    void TriggerNak(uint8_t vc_id, uint16_t seq);
    // 2. 发送前调用，尝试将 ACK 贴在包上
    void PiggybackAck(Ptr<Packet> p);
    //******************************************************************************** */
    bool m_is_retransmitting;//表示当前是否是重传状态
    // --- 新增状态变量 ---发送端要用的
    Ptr<Packet> m_currentLargePacket; // 当前正在被切片的大包
    uint32_t m_currentFlitIdx;        // 当前切到了第几个 Flit
    uint32_t m_currentleftsize;      // 当前大包还剩多少字节没切
    uint32_t m_totalFlits;            // 这个大包总共要切多少个 Flit
    uint32_t m_totalBytes;           // 这个大包的总字节数
    //uint32_t m_credits;          // 当前剩余的发送 Credits
    // 物理 Flit 大小 (建议做成 Attribute，这里先写死示例)
    const uint32_t m_flitSize = 256; 
    const uint16_t MAX_VALID_WINDOW = 32768;
    // --- 新增辅助函数 ---
    void SendNextFlit(); // 负责切一刀并发送，端侧用的
    //void SendNextFlit_switch(); // 负责切一刀并发送，交换机用的
    // 【新增】交换机 HoL 暂存区及上下文
    Ptr<Packet> m_switchPendingPkt;        // 暂存的包
    bool        m_switchPendingIsSwitched; // 它是转发包(true)还是重传包(false)?
    uint32_t    m_switchPendingQIndex;     // 它来自哪个队列?

    //void SendStandaloneAckIfNeeded();
   
    // 【新增】绝对值流控 - 发送侧 (我是 Sender)
    uint16_t m_txTotalSent;       // 我累计发送了多少 Flit
    uint16_t m_txLimit;           // Switch 允许我发到的上限 (Flit)
    uint16_t m_remoteBufferSize;  // Switch 的静态队列大小 (Flit)
    bool creditflag;//表示现在是有待发送给接收端的信用更新的
    // 【新增】绝对值流控 - 接收侧 (我是 Receiver)
    // 记录我这边累计接收/释放了多少，准备告诉对方
    uint64_t m_rxCumulativeFreed; 
    void piggycredit(Ptr<Packet> p);//捎带credit的函数
    // 【关键】记录上次收到 Credit 的值 (用于处理 16位 回绕)
    uint16_t m_lastReceivedCreditLimit;
   void ReleaseRxCredit (uint16_t flitsFreed);//device用来更新信用变量的函数
    
   //************************************************************************************ */
   //sr要维护的变量，发送端
   uint32_t m_txUna;//窗口左边缘，最早发出去但是还没收到确认的SX
   
   uint16_t m_next_seq_num;//下一个要分配的flit的序号
   //uint32_t m_windowsize;//发送窗口大小
   Ptr<ReplayBuffer> m_replayBuffer;//重传缓冲区
   //接收端
   
   Ptr<RxBuffer> m_rxBuffer;//这个是重排序缓冲区
   // 这就是你的"本地位图"，它长久存在
    std::vector<bool> m_isReceived;
    uint16_t m_rxNext;       // 【ACK 指针】：连续收到的最高序号 + 1
    uint16_t m_forwardNext;  // 【转发指针】：等待进入交换机的队头
    // 【新增】记录上次 NAK 的 FirstMissing 序号
    // 用于判断是否是“新的故障”
    uint16_t m_lastNakSeq;
    
    bool cantransmit();//判断当前的重排序缓冲区能否转发包
   // 辅助函数：检查是否过了冷却期
    bool CheckNackCooldown(uint16_t currentMissingSeq);

    // 状态变量
    Time m_lastNackTime;   // 上次发送 NACK 的时刻
    Time m_nackCooldown;   // 冷却时间间隔 (通常设置为 1个 RTT)

    // bool nakflag;//表示当前有nak要发送
    // uint16_t nakseq;//表示当前要发送的nak序号
    // uint32_t nakbitmap;//表示当前要发送的nak位图
    bool nakflag;
    uint16_t nakseq;
    uint64_t nakbitmapHigh;  // 替换原来的 uint32_t nakbitmap
    uint64_t nakbitmapLow;
     void UpdateRtoTimer();
     void HandleRtoTimeout ();
     Time m_rtoBase;  // 必须在这里
     Time m_rtoValue;
     EventId m_rtoEvent;
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
  virtual bool SwitchSend (uint32_t qIndex, Ptr<Packet> packet,CustomHeader &ch);

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

   void SetQueue (Ptr<BEgressQueue> q);
   Ptr<BEgressQueue> GetQueue ();
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
   Ptr<BEgressQueue> m_queue;

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
