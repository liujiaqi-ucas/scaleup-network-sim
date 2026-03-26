#include "switch-mmu.h"

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
#include "switch-node.h" // 【修改1】必须在这里 include，否则不能调用 m_node 的函数
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
      m_minGuarantee(0), m_alpha(SWITCH_MMU_ALPHA) {
    // 构造时只做最小初始化，池子由 ConfigPool() 延迟创建
    m_portUsed.resize(pCnt, 0);
}

void SwitchMmu::ConfigPool(uint32_t poolSize, uint32_t minGuarantee) {
    m_totalPoolSize = poolSize;
    m_poolFree = poolSize;
    m_minGuarantee = minGuarantee;

    // 给物理池分配空间
    m_physicalSRAM.resize(poolSize, nullptr);

    // 清空旧的空闲列表 (防止重复调用)
    while (!m_freeList.empty()) m_freeList.pop();

    // 把所有物理下标放进空闲管家盒子
    for (uint32_t i = 0; i < poolSize; i++) {
        m_freeList.push(i);
    }

    NS_LOG_INFO("SwitchMMU ConfigPool: Total=" << poolSize << ", MinG=" << minGuarantee);
}
// 【修改3】实现 SetNode
void SwitchMmu::SetNode(SwitchNode* node) {
    m_node = node;
}
// =========================================================
// 核心：动态阈值裁决与内存分配 (修复了类名 SwitchMMU -> SwitchMmu)
// =========================================================
int SwitchMmu::AllocateSpace(uint32_t portId) {
    if (m_poolFree == 0 || m_freeList.empty()) {
        return -1; // 物理枯竭
    }

    uint32_t currentUsed = m_portUsed[portId];

    // 步骤 1：查保底额度 (B_min)
    if (currentUsed < m_minGuarantee) {
        m_portUsed[portId]++;
        m_poolFree--;
        int index = m_freeList.front();
        m_freeList.pop();
        return index;
    }

    // 步骤 2：超出了保底额度，计算动态阈值
    uint32_t dynamicThreshold = m_minGuarantee + static_cast<uint32_t>(m_alpha * m_poolFree);

    // 步骤 3：阈值判断
    if (currentUsed < dynamicThreshold) {
        m_portUsed[portId]++;
        m_poolFree--;
        int index = m_freeList.front();
        m_freeList.pop();
        return index;
    }

    // 拒绝入库！触发反压
    NS_LOG_DEBUG("Port " << portId << " admission rejected. Used=" << currentUsed << ", Threshold=" << dynamicThreshold);
    return -1; 
}
void SwitchMmu::StorePacket(int index, Ptr<Packet> p) {
    m_physicalSRAM[index] = p;
}

Ptr<Packet> SwitchMmu::ReadFlit(int index) const {
    return m_physicalSRAM[index];
}

void SwitchMmu::FreeSpace(int index, uint32_t portId) {
    // 1. 彻底抹除智能指针引用
    m_physicalSRAM[index] = nullptr;

    // 2. 归还钥匙
    m_freeList.push(index);

    // 3. 归还账本
    if (m_portUsed[portId] > 0) {
        m_portUsed[portId]--;
    }
    m_poolFree++;
}

uint32_t SwitchMmu::GetPoolFree() const {
    return m_poolFree;
}


}  // namespace ns3
