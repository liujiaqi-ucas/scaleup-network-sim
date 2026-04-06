#ifndef SWITCH_MMU_H
#define SWITCH_MMU_H

#include <ns3/node.h>
#include <ns3/random-variable-stream.h>

#include <list>
#include <unordered_map>

#include "ns3/conga-routing.h"
#include "ns3/conweave-routing.h"
#include "ns3/letflow-routing.h"
#include "ns3/settings.h"

#define SWITCH_MMU_ALPHA 0.5   // α 初始值 / 上限

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

    // 运行时配置：创建交换机后调用
    void ConfigPool(uint32_t poolSize, uint32_t minGuarantee);

    // =========================================================
    // 核心功能
    // =========================================================
    int  AllocateSpace(uint32_t portId);          // 申请槽位（动态阈值准入）
    void StorePacket(int index, Ptr<Packet> p);   // 存入 Payload
    Ptr<Packet> ReadFlit(int index) const;         // 零拷贝读取
    void FreeSpace(int index, uint32_t portId);    // 释放槽位（ACK 时调用）

    // =========================================================
    // 动态 Alpha：重传缓冲区感知
    // =========================================================
    void MarkAsSent(int slotIndex, uint32_t portId);  // flit 发送上线路时调用
    void EvaluatePortAlpha();                          // 周期性 AIMD 评估

    uint32_t GetPoolFree() const;
    uint32_t GetPortUsed(uint32_t portId) const { return portId < m_portUsed.size() ? m_portUsed[portId] : 0; }

    /*------------ Routing Objects (public) -------------*/
    CongaRouting m_congaRouting;
    LetflowRouting m_letflowRouting;
    ConWeaveRouting m_conweaveRouting;

   private:
    SwitchNode* m_node;

    // --- 物理共享池 ---
    uint32_t m_totalPoolSize;
    uint32_t m_poolFree;
    uint32_t m_minGuarantee;

    std::vector<Ptr<Packet>> m_physicalSRAM;   // 物理池
    std::queue<int>          m_freeList;        // 空闲下标
    std::vector<uint32_t>    m_portUsed;        // 每出端口总占用（转发队列+重传缓冲区）

    // --- 动态 Alpha 相关 ---
    double   m_portAlpha[pCnt];          // 每出端口独立的 α
    uint32_t m_portRetransBuf[pCnt];     // 每出端口 已发未确认 的 flit 数
    bool*    m_slotIsSent;               // 每个槽位是否已发出 (大小=poolSize)

    bool     m_evalScheduled;    // 是否已调度 EvaluatePortAlpha

    // AIMD 参数
    double   m_alphaMax;        // α 上限 (= SWITCH_MMU_ALPHA, 0.5)
    double   m_alphaMin;        // α 下限 (0.1)
    double   m_mdBeta;          // 乘法减因子 (0.5)
    double   m_aiDelta;         // 加法增步长 (0.05)
    double   m_retransThresh;   // 重传缓冲区比例阈值 (0.7)
    uint32_t m_evalMinUsed;     // 冷启动保护：portUsed < 此值时不评估 (8)
    Time     m_evalInterval;    // 评估周期 (10μs)
};

} /* namespace ns3 */

#endif /* SWITCH_MMU_H */
