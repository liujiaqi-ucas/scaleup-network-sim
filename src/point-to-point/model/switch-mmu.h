#ifndef SWITCH_MMU_H
#define SWITCH_MMU_H

#include <ns3/node.h>
#include <ns3/random-variable-stream.h>

#include <list>
#include <set>
#include <unordered_map>

#include "ns3/conga-routing.h"
#include "ns3/conweave-routing.h"
#include "ns3/letflow-routing.h"
#include "ns3/settings.h"

#define SWITCH_MMU_ALPHA 0.5

namespace ns3 {

class Packet;
class SwitchNode;

class SwitchMmu : public Object {
   public:
    static const unsigned pCnt = 128;

    void SetNode(SwitchNode* node);
    static TypeId GetTypeId(void);

    SwitchMmu(void);
    virtual ~SwitchMmu(void);

    void ConfigPool(uint32_t poolSize, uint32_t minGuarantee);

    int  AllocateSpace(uint32_t portId);
    void StorePacket(int index, Ptr<Packet> p);
    Ptr<Packet> ReadFlit(int index) const;
    void FreeSpace(int index, uint32_t portId);

    void MarkAsSent(int slotIndex, uint32_t portId);
    void EvaluatePortAlpha();

    uint32_t GetPoolFree() const;
    uint32_t GetPortUsed(uint32_t portId) const { return portId < m_portUsed.size() ? m_portUsed[portId] : 0; }

    // --- Egress-based PFC ---
    void ConfigPfcThresholds(uint32_t xoffThreshold, uint32_t xonThreshold);
    bool CheckEgressPfc(uint32_t txPortId) const;    // egress port > XOFF?
    bool CheckEgressResume(uint32_t txPortId) const;  // egress port < XON?
    uint32_t m_pfcXoffThreshold;
    uint32_t m_pfcXonThreshold;
    bool m_egressInPfc[pCnt];                         // 是否处于 PFC PAUSE 状态
    std::set<int> m_egressPfcPausedRx[pCnt];          // 每个 egress port 暂停了哪些 RX 端口

    CongaRouting m_congaRouting;
    LetflowRouting m_letflowRouting;
    ConWeaveRouting m_conweaveRouting;

   private:
    SwitchNode* m_node;

    uint32_t m_totalPoolSize;
    uint32_t m_poolFree;
    uint32_t m_minGuarantee;

    std::vector<Ptr<Packet>> m_physicalSRAM;
    std::queue<int>          m_freeList;
    std::vector<uint32_t>    m_portUsed;

    // --- 动态 Alpha：重传缓冲区占比感知 ---
    double   m_portAlpha[pCnt];
    uint32_t m_portRetransBuf[pCnt];
    bool*    m_slotIsSent;
    uint32_t m_portWarmup[pCnt];
    bool     m_portWasActive[pCnt];

    bool     m_evalScheduled;

    double   m_alphaMax;
    double   m_mdBeta;
    double   m_aiDelta;
    double   m_retransThresh;
    uint32_t m_evalMinUsed;
    uint32_t m_warmupPeriods;
    Time     m_evalInterval;
};

} /* namespace ns3 */

#endif /* SWITCH_MMU_H */
