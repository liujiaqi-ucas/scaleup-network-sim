#ifndef RDMA_HW_H
#define RDMA_HW_H

#include <ns3/custom-header.h>
#include <ns3/node.h>
#include <ns3/rdma.h>
#include <ns3/selective-packet-queue.h>

#include <unordered_map>
#include <unordered_set>

#include "qbb-net-device.h"
#include "rdma-queue-pair.h"
#define PROTOCOL_HEADER_SIZE 48
namespace ns3 {

struct RdmaInterfaceMgr {
    Ptr<QbbNetDevice> dev;
    Ptr<RdmaQueuePairGroup> qpGrp;

    RdmaInterfaceMgr() : dev(NULL), qpGrp(NULL) {}
    RdmaInterfaceMgr(Ptr<QbbNetDevice> _dev) { dev = _dev; }
};
typedef Callback<void, Ptr<RdmaRxQueuePair>, double> RxFlowCompleteCallback;
class RdmaHw : public Object {
   public:
   RxFlowCompleteCallback m_rxFlowCompleteCb;
    //void DeleteRxQp(uint32_t dip, uint16_t dport, uint16_t sport, uint16_t pg);
    //定义设置函数
    void SetRxFlowCompleteCallback(RxFlowCompleteCallback cb) {
        m_rxFlowCompleteCb = cb;
    }
    static TypeId GetTypeId(void);
    RdmaHw();
    //Ptr<RdmaRxQueuePair> m_currentRxQp; // 用于缓存当前正在接收的 QP
    std::unordered_map<uint32_t, Ptr<RdmaRxQueuePair>> m_currentRxQpPerDev; 
    Ptr<Node> m_node;
    DataRate m_minRate;  //< Min sending rate
    uint32_t m_mtu;
    uint32_t m_cc_mode;
    //double m_nack_interval;
    uint32_t m_chunk;
    uint32_t m_ack_interval;
    bool m_backto0;
    bool m_var_win, m_fast_react;
    bool m_rateBound;
    std::vector<RdmaInterfaceMgr> m_nic;  // list of running nic controlled by this RdmaHw
    std::unordered_map<uint64_t, Ptr<RdmaQueuePair>> m_qpMap;      // mapping from uint64_t to qp
    std::unordered_map<uint64_t, Ptr<RdmaRxQueuePair>> m_rxQpMap;  // mapping from uint64_t to rx qp
    std::unordered_map<uint32_t, std::vector<int>>
        m_rtTable;  // map from ip address (u32) to possible ECMP port (index of dev)

    // qp complete callback
    typedef Callback<void, Ptr<RdmaQueuePair>> QpCompleteCallback;
    QpCompleteCallback m_qpCompleteCallback;

    void SetNode(Ptr<Node> node);
    void Setup(QpCompleteCallback cb);  // setup shared data and callbacks with the QbbNetDevice

    /* Akashic Record of finished QP */
    std::unordered_set<uint64_t> akashic_Qp;    // instance for each src
    std::unordered_set<uint64_t> akashic_RxQp;  // instance for each dst
    static uint64_t nAllPkts;                   // number of total packets

    /* TxQpeueuPair */
    static uint64_t GetQpKey(uint32_t dip, uint16_t sport, uint16_t dport,
                             uint16_t pg);          // get the lookup key for m_qpMap
    Ptr<RdmaQueuePair> GetQp(uint64_t key);         // get the qp
    uint32_t GetNicIdxOfQp(Ptr<RdmaQueuePair> qp);  // get the NIC index of the qp
    void DeleteQueuePair(Ptr<RdmaQueuePair> qp);    // delete TxQP

    void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip,
                      uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt,
                      int32_t flow_id);  // add a nw qp (new send)
    void AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address _sip, Ipv4Address _dip,
                      uint16_t _sport, uint16_t _dport, uint32_t win, uint64_t baseRtt) {
        this->AddQueuePair(size, pg, _sip, _dip, _sport, _dport, win, baseRtt, -1);
    }

    /* RxQueuePair */
    static uint64_t GetRxQpKey(uint32_t dip, uint16_t dport, uint16_t sport, uint16_t pg);
    Ptr<RdmaRxQueuePair> GetRxQp(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport,
                                 uint16_t pg, bool create);  // get a rxQp
    uint32_t GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q);        // get the NIC index of the rxQp
    void DeleteRxQp(uint32_t dip, uint16_t dport, uint16_t sport, uint16_t pg);  // delete RxQP

    int ReceiveUdp(Ptr<Packet> p, CustomHeader &ch, uint32_t dev_idx);
    
    int Receive(Ptr<Packet> p,
                CustomHeader &
                    ch, uint32_t dev_idx);  // callback function that the QbbNetDevice should use when receive
                          // packets. Only NIC can call this function. And do not call this upon PFC

    
    
    void AddHeader(Ptr<Packet> p, uint16_t protocolNumber);
    static uint16_t EtherToPpp(uint16_t protocol);

    
    void QpComplete(Ptr<RdmaQueuePair> qp);
    void SetLinkDown(Ptr<QbbNetDevice> dev);

    // call this function after the NIC is setup
    void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
    void ClearTable();
    void RedistributeQp();

    Ptr<Packet> GetNxtPacket(Ptr<RdmaQueuePair> qp);  // get next packet to send, inc snd_nxt
    void PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap);
    void UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size);
    void ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate);

    void HandleTimeout(Ptr<RdmaQueuePair> qp, Time rto);

    

    
    
};

} /* namespace ns3 */

#endif /* RDMA_HW_H */
