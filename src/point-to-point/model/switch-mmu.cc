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
      m_minGuarantee(0),
      m_globalAlpha(SWITCH_MMU_ALPHA),
      m_evalScheduled(false) {
    m_portUsed.resize(pCnt, 0);
}

SwitchMmu::~SwitchMmu(void) {
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

    NS_LOG_INFO("SwitchMMU ConfigPool: Total=" << poolSize
                << ", MinG=" << minGuarantee
                << ", GlobalAlpha=" << m_globalAlpha);
}

void SwitchMmu::SetNode(SwitchNode* node) {
    m_node = node;
}

// =========================================================
// 核心：动态阈值准入（使用全局固定 α）
// =========================================================
int SwitchMmu::AllocateSpace(uint32_t portId) {
    if (m_poolFree == 0 || m_freeList.empty()) {
        return -1;
    }

    uint32_t currentUsed = m_portUsed[portId];

    // 步骤 1：保底额度内直接放行
    if (currentUsed < m_minGuarantee) {
        int index = m_freeList.front();
        m_freeList.pop();
        m_portUsed[portId]++;
        m_poolFree--;
        return index;
    }

    // 步骤 2：超出保底，用全局固定 α 计算动态阈值
    uint32_t dynamicPart = std::max(1u,
        static_cast<uint32_t>(m_globalAlpha * m_poolFree));
    uint32_t dynamicThreshold = m_minGuarantee + dynamicPart;

    // 步骤 3：准入判定
    if (currentUsed < dynamicThreshold) {
        int index = m_freeList.front();
        m_freeList.pop();
        m_portUsed[portId]++;
        m_poolFree--;
        return index;
    }

    NS_LOG_DEBUG("Port " << portId << " rejected. Used=" << currentUsed
                 << ", Thresh=" << dynamicThreshold
                 << ", GlobalAlpha=" << m_globalAlpha);
    return -1;
}

void SwitchMmu::StorePacket(int index, Ptr<Packet> p) {
    m_physicalSRAM[index] = p;
}

Ptr<Packet> SwitchMmu::ReadFlit(int index) const {
    return m_physicalSRAM[index];
}

// =========================================================
// 释放槽位（ACK 时调用）
// =========================================================
void SwitchMmu::FreeSpace(int index, uint32_t portId) {
    if (index < 0 || index >= (int)m_totalPoolSize) return;
    if (m_poolFree >= m_totalPoolSize) {
        NS_LOG_ERROR("FreeSpace: pool overflow detected! index=" << index
                     << " portId=" << portId);
        return;
    }

    m_physicalSRAM[index] = nullptr;
    m_freeList.push(index);

    if (m_portUsed[portId] > 0) {
        m_portUsed[portId]--;
    }
    m_poolFree++;
}

// =========================================================
// 基线方案：空操作（全局固定 α 不需要追踪重传状态）
// =========================================================
void SwitchMmu::MarkAsSent(int slotIndex, uint32_t portId) {
    (void)slotIndex; (void)portId;
}

void SwitchMmu::EvaluatePortAlpha() {
    // 基线：α 固定不变，无需 AIMD 调整
}

uint32_t SwitchMmu::GetPoolFree() const {
    return m_poolFree;
}

}  // namespace ns3
