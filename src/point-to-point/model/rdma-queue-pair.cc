#include "rdma-queue-pair.h"

#include <ns3/hash.h>
#include <ns3/ipv4-header.h>
#include <ns3/log.h>
#include <ns3/seq-ts-header.h>
#include <ns3/simulator.h>
#include <ns3/udp-header.h>
#include <ns3/uinteger.h>

#include "ns3/ppp-header.h"
#include "ns3/settings.h"
#include "rdma-hw.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RdmaQueuePair");

/**************************
 * RdmaQueuePair
 *************************/
TypeId RdmaQueuePair::GetTypeId(void) {
    static TypeId tid = TypeId("ns3::RdmaQueuePair").SetParent<Object>();
    return tid;
}

RdmaQueuePair::RdmaQueuePair(uint16_t pg, Ipv4Address _sip, Ipv4Address _dip, uint16_t _sport,
                             uint16_t _dport) {
    startTime = Simulator::Now();
    sip = _sip;
    dip = _dip;
    sport = _sport;
    dport = _dport;
    m_size = 0;
    
    snd_nxt = 0;
    m_pg = pg;
    m_ipid = 0;
    
    m_max_rate = 0;
   
    m_rate = 0;
    m_nextAvail = Time(0);
    //m_next_byte_seq = 0; // 假设流从 0 开始
    
}

void RdmaQueuePair::SetSize(uint64_t size) { m_size = size; }

void RdmaQueuePair::SetFlowId(int32_t v) {
    m_flow_id = v;
   
}

//void RdmaQueuePair::SetTimeout(Time v) { m_timeout = v; }

uint64_t RdmaQueuePair::GetBytesLeft() {

    return m_size >= snd_nxt ? m_size - snd_nxt : 0;//什么时候更新？
}

uint32_t RdmaQueuePair::GetHash(void) {
    union {
        struct {
            uint32_t sip, dip;
            uint16_t sport, dport;
        };
        char c[12];
    } buf;
    buf.sip = sip.Get();
    buf.dip = dip.Get();
    buf.sport = sport;
    buf.dport = dport;
    return Hash32(buf.c, 12);
}

bool RdmaQueuePair::IsFinished() {
    std::cout<<"qp  finished"<<std::endl;
    return snd_nxt >= m_size;
}

/*********************
 * RdmaRxQueuePair
 ********************/
TypeId RdmaRxQueuePair::GetTypeId(void) {
    static TypeId tid = TypeId("ns3::RdmaRxQueuePair").SetParent<Object>();
    return tid;
}

RdmaRxQueuePair::RdmaRxQueuePair() {
    sip = dip = sport = dport = 0;
    m_ipid = 0;
    expected_seq=0;
}

uint32_t RdmaRxQueuePair::GetHash(void) {
    union {
        struct {
            uint32_t sip, dip;
            uint16_t sport, dport;
        };
        char c[12];
    } buf;
    buf.sip = sip;
    buf.dip = dip;
    buf.sport = sport;
    buf.dport = dport;
    return Hash32(buf.c, 12);
}

/*********************
 * RdmaQueuePairGroup
 ********************/
TypeId RdmaQueuePairGroup::GetTypeId(void) {
    static TypeId tid = TypeId("ns3::RdmaQueuePairGroup").SetParent<Object>();
    return tid;
}

RdmaQueuePairGroup::RdmaQueuePairGroup(void) { memset(m_qp_finished, 0, sizeof(m_qp_finished)); }

uint32_t RdmaQueuePairGroup::GetN(void) { return m_qps.size(); }

Ptr<RdmaQueuePair> RdmaQueuePairGroup::Get(uint32_t idx) { return m_qps[idx]; }

Ptr<RdmaQueuePair> RdmaQueuePairGroup::operator[](uint32_t idx) { return m_qps[idx]; }

void RdmaQueuePairGroup::AddQp(Ptr<RdmaQueuePair> qp) { m_qps.push_back(qp); }

// void RdmaQueuePairGroup::AddRxQp(Ptr<RdmaRxQueuePair> rxQp){
// 	m_rxQps.push_back(rxQp);
// }

void RdmaQueuePairGroup::Clear(void) { m_qps.clear(); }







}  // namespace ns3
