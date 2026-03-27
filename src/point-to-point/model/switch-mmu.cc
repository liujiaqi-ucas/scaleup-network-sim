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
      m_minGuarantee(0), m_slotIsSent(nullptr),
      m_evalScheduled(false),
      m_alphaMax(SWITCH_MMU_ALPHA), m_alphaMin(0.1),
      m_mdBeta(0.5), m_aiDelta(0.05),
      m_retransThresh(0.7), m_evalMinUsed(8),
      m_evalInterval(MicroSeconds(10)) {
    m_portUsed.resize(pCnt, 0);
    for (uint32_t i = 0; i < pCnt; i++) {
        m_portAlpha[i] = m_alphaMax;
        m_portRetransBuf[i] = 0;
    }
}

SwitchMmu::~SwitchMmu(void) {
    if (m_slotIsSent) {
        delete[] m_slotIsSent;
        m_slotIsSent = nullptr;
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

    // 初始化槽位元数据
    if (m_slotIsSent) delete[] m_slotIsSent;
    m_slotIsSent = new bool[poolSize]();  // 零初始化

    // 重置每端口状态
    for (uint32_t i = 0; i < pCnt; i++) {
        m_portAlpha[i] = m_alphaMax;
        m_portRetransBuf[i] = 0;
    }

    // 启动周期性评估定时器（防止重复调用 ConfigPool 导致多个定时器）
    if (!m_evalScheduled) {
        Simulator::Schedule(m_evalInterval, &SwitchMmu::EvaluatePortAlpha, this);
        m_evalScheduled = true;
    }

    NS_LOG_INFO("SwitchMMU ConfigPool: Total=" << poolSize
                << ", MinG=" << minGuarantee
                << ", AlphaMax=" << m_alphaMax);
}

void SwitchMmu::SetNode(SwitchNode* node) {
    m_node = node;
}

// =========================================================
// 核心：动态阈值准入（使用 per-port α）
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
        m_slotIsSent[index] = false;
        return index;
    }

    // 步骤 2：超出保底，用该端口自己的 α 计算动态阈值
    // 保底 +1：防止 α × poolFree 截断为 0 导致阈值退化
    uint32_t dynamicPart = std::max(1u,
        static_cast<uint32_t>(m_portAlpha[portId] * m_poolFree));
    uint32_t dynamicThreshold = m_minGuarantee + dynamicPart;

    // 步骤 3：准入判定
    if (currentUsed < dynamicThreshold) {
        int index = m_freeList.front();
        m_freeList.pop();
        m_portUsed[portId]++;
        m_poolFree--;
        m_slotIsSent[index] = false;
        return index;
    }

    NS_LOG_DEBUG("Port " << portId << " rejected. Used=" << currentUsed
                 << ", Thresh=" << dynamicThreshold
                 << ", Alpha=" << m_portAlpha[portId]);
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

    // 同步重传缓冲区计数
    if (m_slotIsSent[index]) {
        if (m_portRetransBuf[portId] > 0) {
            m_portRetransBuf[portId]--;
        }
        m_slotIsSent[index] = false;
    }
}

// =========================================================
// flit 发送上线路时调用：标记槽位为 "已发未确认"
// =========================================================
void SwitchMmu::MarkAsSent(int slotIndex, uint32_t portId) {
    if (slotIndex < 0 || slotIndex >= (int)m_totalPoolSize) return;
    if (!m_slotIsSent[slotIndex]) {
        m_slotIsSent[slotIndex] = true;
        m_portRetransBuf[portId]++;
    }
}

// =========================================================
// 周期性 AIMD 评估：根据重传缓冲区占比调整 α
// =========================================================
void SwitchMmu::EvaluatePortAlpha() {
    for (uint32_t p = 0; p < pCnt; p++) {
        // 冷启动保护
        if (m_portUsed[p] < m_evalMinUsed) {
            continue;
        }

        double ratio = static_cast<double>(m_portRetransBuf[p])
                      / static_cast<double>(m_portUsed[p]);

        if (ratio > m_retransThresh) {
            // 乘法减：重传缓冲区占比过高
            m_portAlpha[p] = std::max(m_alphaMin, m_portAlpha[p] * m_mdBeta);
        } else {
            // 加法增：恢复
            m_portAlpha[p] = std::min(m_alphaMax, m_portAlpha[p] + m_aiDelta);
        }
    }

    // 调度下一次评估
    Simulator::Schedule(m_evalInterval, &SwitchMmu::EvaluatePortAlpha, this);
}

uint32_t SwitchMmu::GetPoolFree() const {
    return m_poolFree;
}

}  // namespace ns3
