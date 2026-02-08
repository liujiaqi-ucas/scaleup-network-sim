#ifndef RDMA_QUEUE_PAIR_H
#define RDMA_QUEUE_PAIR_H

#include <ns3/custom-header.h>
#include <ns3/data-rate.h>
#include <ns3/event-id.h>
#include <ns3/int-header.h>
#include <ns3/ipv4-address.h>
#include <ns3/object.h>
#include <ns3/packet.h>
#include <ns3/selective-packet-queue.h>

#include <climits> /* for CHAR_BIT */
#include <vector>

#define BITMASK(b) (1 << ((b) % CHAR_BIT))
#define BITSLOT(b) ((b) / CHAR_BIT)
#define BITSET(a, b) ((a)[BITSLOT(b)] |= BITMASK(b))
#define BITCLEAR(a, b) ((a)[BITSLOT(b)] &= ~BITMASK(b))
#define BITTEST(a, b) ((a)[BITSLOT(b)] & BITMASK(b))
#define BITNSLOTS(nb) ((nb + CHAR_BIT - 1) / CHAR_BIT)

#define ESTIMATED_MAX_FLOW_PER_HOST 9120

namespace ns3 {


class RdmaQueuePair : public Object {
   public:
    Time startTime;
    Ipv4Address sip, dip;
    uint16_t sport, dport;
    uint64_t m_size;
    uint64_t snd_nxt;//, snd_una;  // next seq to send, the highest unacked seq
    uint16_t m_pg;
    uint16_t m_ipid;
    
    DataRate m_max_rate;  // max rate
    
    Time m_nextAvail;     //< Soonest time of next send
    
    uint32_t lastPktSize;
    int32_t m_flow_id;
    

    /******************************
     * runtime states
     *****************************/
    DataRate m_rate;  //< Current rate
    
    struct {
        uint64_t txTotalPkts{0};
        uint64_t txTotalBytes{0};
    } stat;

    // Implement Timeout according to IB Spec Vol. 1 C9-139.
    // For an HCA requester using Reliable Connection service, to detect missing responses,
    // every Send queue is required to implement a Transport Timer to time outstanding requests.
    //EventId m_retransmit;

    /***********
     * methods
     **********/
    static TypeId GetTypeId(void);
    RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport,
                  uint16_t _dport);
    void SetSize(uint64_t size);
    void SetFlowId(int32_t v);
    uint64_t GetBytesLeft();
    uint32_t GetHash(void);
    inline bool IsFinishedConst() const { return snd_nxt >= m_size;}
    // 核心逻辑：发送端是否发完了？(注意：发完不代表对面收完)
    bool IsFinished();
    
};

class RdmaRxQueuePair : public Object {  // Rx side queue pair
   public:
    uint32_t sip, dip;
    uint16_t sport, dport;
    uint16_t m_ipid;
    int32_t m_flow_id;
   uint32_t expected_seq; // 期望收到的下一个序号
    // ==========================================
    // 【新增】存储流的总大小
    // ==========================================
    uint64_t m_size;
    // 【新增】实际接收到的纯数据字节数 (Accumulator)
    uint64_t received_bytes;
    static TypeId GetTypeId(void);
    RdmaRxQueuePair();
    uint32_t GetHash(void);
    
};

class RdmaQueuePairGroup : public Object {
   public:
    std::vector<Ptr<RdmaQueuePair>> m_qps;
    // std::vector<Ptr<RdmaRxQueuePair> > m_rxQps;
    char m_qp_finished[BITNSLOTS(ESTIMATED_MAX_FLOW_PER_HOST)];

    static TypeId GetTypeId(void);
    RdmaQueuePairGroup(void);
    uint32_t GetN(void);
    Ptr<RdmaQueuePair> Get(uint32_t idx);
    Ptr<RdmaQueuePair> operator[](uint32_t idx);
    void AddQp(Ptr<RdmaQueuePair> qp);
    // void AddRxQp(Ptr<RdmaRxQueuePair> rxQp);
    void Clear(void);
    inline bool IsQpFinished(uint32_t idx) {
        if (__glibc_unlikely(idx >= ESTIMATED_MAX_FLOW_PER_HOST)) return false;
        return BITTEST(m_qp_finished, idx);
    }

    inline void SetQpFinished(uint32_t idx) {
        if (__glibc_unlikely(idx >= ESTIMATED_MAX_FLOW_PER_HOST)) return;
        BITSET(m_qp_finished, idx);
    }
};

}  // namespace ns3

#endif /* RDMA_QUEUE_PAIR_H */
