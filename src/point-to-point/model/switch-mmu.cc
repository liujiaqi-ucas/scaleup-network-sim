#include "switch-mmu.h"

#include <algorithm>
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
#include "switch-node.h"
#include "qbb-net-device.h"

NS_LOG_COMPONENT_DEFINE("SwitchMmu");

namespace ns3 {

TypeId SwitchMmu::GetTypeId(void) {
    static TypeId tid =
        TypeId("ns3::SwitchMmu")
            .SetParent<Object>()
            .AddConstructor<SwitchMmu>();
    return tid;
}

SwitchMmu::SwitchMmu(void)
    : m_node(nullptr), m_totalPoolSize(0), m_poolFree(0),
      m_minGuarantee(0), m_slotIsSent(nullptr), m_slotIsHeadroom(nullptr),
      m_evalScheduled(false),
      m_alphaMax(SWITCH_MMU_ALPHA),
      m_mdBeta(0.5), m_aiDelta(0.05),
      m_retransThresh(0.5), m_evalMinUsed(8),
      m_warmupPeriods(5),
      m_evalInterval(MicroSeconds(10)) {
    m_portUsed.resize(pCnt, 0);
    m_pfcXoffThreshold = 0;
    m_pfcXonThreshold = 0;
    for (uint32_t i = 0; i < pCnt; i++) {
        m_portAlpha[i]      = m_alphaMax;
        m_portRetransBuf[i] = 0;
        m_portWarmup[i]     = 0;
        m_portWasActive[i]  = false;
        m_egressInPfc[i]    = false;
        m_headroomPerPort[i]= 0;
        m_headroomUsed[i]   = 0;
    }
}

SwitchMmu::~SwitchMmu(void) {
    if (m_slotIsSent) {
        delete[] m_slotIsSent;
        m_slotIsSent = nullptr;
    }
    if (m_slotIsHeadroom) {
        delete[] m_slotIsHeadroom;
        m_slotIsHeadroom = nullptr;
    }
}

void SwitchMmu::ConfigPool(uint32_t poolSize, uint32_t minGuarantee) {
    m_totalPoolSize = poolSize;
    m_poolFree = poolSize;
    m_minGuarantee = minGuarantee;

    m_physicalSRAM.resize(poolSize, nullptr);

    while (!m_freeList.empty()) m_freeList.pop();
    for (uint32_t i = 0; i < poolSize; i++) {
        m_freeList.push(i);
    }

    if (m_slotIsSent) delete[] m_slotIsSent;
    m_slotIsSent = new bool[poolSize]();

    if (m_slotIsHeadroom) delete[] m_slotIsHeadroom;
    m_slotIsHeadroom = new bool[poolSize]();

    for (uint32_t i = 0; i < pCnt; i++) {
        m_portAlpha[i]      = m_alphaMax;
        m_portRetransBuf[i] = 0;
        m_portWarmup[i]     = 0;
        m_portWasActive[i]  = false;
        m_headroomUsed[i]   = 0;
        // headroomPerPort 由 ConfigPortHeadroom 设置，此处保留已有值
    }

    if (!m_evalScheduled) {
        Simulator::Schedule(m_evalInterval, &SwitchMmu::EvaluatePortAlpha, this);
        m_evalScheduled = true;
    }

    NS_LOG_INFO("SwitchMMU ConfigPool: Total=" << poolSize
                << ", MinG=" << minGuarantee);
}

void SwitchMmu::ConfigPortHeadroom(uint32_t portId, uint32_t headroomFlits) {
    if (portId >= pCnt) return;
    m_headroomPerPort[portId] = headroomFlits;
}

void SwitchMmu::SetNode(SwitchNode* node) {
    m_node = node;
}

// =========================================================
// AllocateSpace: 三阶段分配
//   Phase 1: minGuarantee — 无条件允许（保底）
//   Phase 2: 动态共享池   — 正常转发路径
//   Phase 3: Headroom     — 仅在 PFC PAUSE 状态下可用
//            接收 PAUSE 后仍在途中的 flit 用此空间落地，
//            确保 PFC 无损语义，总量不超过 m_headroomPerPort[portId]
// =========================================================
int SwitchMmu::AllocateSpace(uint32_t portId) {
    if (m_poolFree == 0 || m_freeList.empty()) return -1;

    uint32_t currentUsed = m_portUsed[portId];

    // Phase 1: 最低保证
    if (currentUsed < m_minGuarantee) goto do_shared_alloc;

    // Phase 2: 动态共享池（正常路径）
    {
        uint32_t dynamicPart = std::max(1u,
            static_cast<uint32_t>(m_portAlpha[portId] * m_poolFree));
        if (currentUsed < m_minGuarantee + dynamicPart) goto do_shared_alloc;
    }

    // Phase 3: Headroom（仅 PFC PAUSE 状态）
    // 上游已被 PAUSE，但链路上在途的 flit 仍会持续到达，
    // 必须有专用空间接收，否则 PFC 无损保证失效。
    if (m_egressInPfc[portId] && m_headroomPerPort[portId] > 0) {
        if (m_headroomUsed[portId] < m_headroomPerPort[portId]) {
            int index = m_freeList.front();
            m_freeList.pop();
            m_portUsed[portId]++;
            m_poolFree--;
            m_slotIsSent[index]    = false;
            m_slotIsHeadroom[index] = true;   // 标记为 headroom 槽
            m_headroomUsed[portId]++;
            return index;
        }
        // headroom 耗尽：理论上不应发生（说明 headroom 配置不足或链路参数有误）
        NS_LOG_WARN("Port " << portId << " headroom exhausted! "
                    << "headroomUsed=" << m_headroomUsed[portId]
                    << " headroomSize=" << m_headroomPerPort[portId]);
    }

    return -1;

do_shared_alloc:
    {
        int index = m_freeList.front();
        m_freeList.pop();
        m_portUsed[portId]++;
        m_poolFree--;
        m_slotIsSent[index]    = false;
        m_slotIsHeadroom[index] = false;
        return index;
    }
}

void SwitchMmu::StorePacket(int index, Ptr<Packet> p) {
    m_physicalSRAM[index] = p;
}

Ptr<Packet> SwitchMmu::ReadFlit(int index) const {
    return m_physicalSRAM[index];
}

void SwitchMmu::FreeSpace(int index, uint32_t portId) {
    if (index < 0 || index >= (int)m_totalPoolSize) return;
    if (m_poolFree >= m_totalPoolSize) {
        NS_LOG_ERROR("FreeSpace: pool overflow! index=" << index << " portId=" << portId);
        return;
    }

    // 如果是 headroom 槽，释放时同步归还 headroom 计数
    if (m_slotIsHeadroom[index]) {
        if (m_headroomUsed[portId] > 0) {
            m_headroomUsed[portId]--;
        }
        m_slotIsHeadroom[index] = false;
    }

    m_physicalSRAM[index] = nullptr;
    m_freeList.push(index);

    if (m_portUsed[portId] > 0) {
        m_portUsed[portId]--;
    }
    m_poolFree++;

    if (m_slotIsSent[index]) {
        if (m_portRetransBuf[portId] > 0) {
            m_portRetransBuf[portId]--;
        }
        m_slotIsSent[index] = false;
    }
}

void SwitchMmu::MarkAsSent(int slotIndex, uint32_t portId) {
    if (slotIndex < 0 || slotIndex >= (int)m_totalPoolSize) return;
    if (!m_slotIsSent[slotIndex]) {
        m_slotIsSent[slotIndex] = true;
        m_portRetransBuf[portId]++;
    }
}

// =========================================================
// 动态 Alpha 调整（不变）
// =========================================================
void SwitchMmu::EvaluatePortAlpha() {
    double poolUsageRatio = 1.0 - static_cast<double>(m_poolFree) / m_totalPoolSize;
    double alphaMin;
    if (poolUsageRatio < 0.5)      alphaMin = 0.3;
    else if (poolUsageRatio < 0.8) alphaMin = 0.2;
    else                           alphaMin = 0.1;

    for (uint32_t p = 0; p < pCnt; p++) {
        bool isActive = (m_portUsed[p] >= m_evalMinUsed);

        if (isActive && !m_portWasActive[p]) {
            m_portWarmup[p] = m_warmupPeriods;
        }
        m_portWasActive[p] = isActive;
        if (!isActive) continue;

        if (m_portWarmup[p] > 0) {
            m_portWarmup[p]--;
            m_portAlpha[p] = std::min(m_alphaMax, m_portAlpha[p] + m_aiDelta);
            continue;
        }

        double retransRatio = static_cast<double>(m_portRetransBuf[p])
                             / static_cast<double>(m_portUsed[p]);

        if (retransRatio > m_retransThresh) {
            m_portAlpha[p] = std::max(alphaMin, m_portAlpha[p] * m_mdBeta);
        } else {
            m_portAlpha[p] = std::min(m_alphaMax, m_portAlpha[p] + m_aiDelta);
        }
    }

    Simulator::Schedule(m_evalInterval, &SwitchMmu::EvaluatePortAlpha, this);
}

uint32_t SwitchMmu::GetPoolFree() const { return m_poolFree; }

// --- Egress-based PFC ---
void SwitchMmu::ConfigPfcThresholds(uint32_t xoffThreshold, uint32_t xonThreshold) {
    m_pfcXoffThreshold = xoffThreshold;
    m_pfcXonThreshold  = xonThreshold;
}

bool SwitchMmu::CheckEgressPfc(uint32_t txPortId) const {
    if (m_pfcXoffThreshold == 0) return false;
    // 触发 XOFF 时只计算共享池部分（不含 headroom）
    uint32_t sharedUsed = m_portUsed[txPortId] - m_headroomUsed[txPortId];
    return sharedUsed >= m_pfcXoffThreshold;
}

bool SwitchMmu::CheckEgressResume(uint32_t txPortId) const {
    if (m_pfcXonThreshold == 0) return false;
    uint32_t sharedUsed = m_portUsed[txPortId] - m_headroomUsed[txPortId];
    return sharedUsed <= m_pfcXonThreshold;
}

}  // namespace ns3
