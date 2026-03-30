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

#define SWITCH_MMU_ALPHA 0.5   // 全局固定 α（基线方案：所有端口共用，不随时间变化）

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
    // 基线方案：全局固定 Alpha（MarkAsSent/EvaluatePortAlpha 保留接口但为空操作）
    // =========================================================
    void MarkAsSent(int slotIndex, uint32_t portId);  // 空操作（基线不需要追踪重传状态）
    void EvaluatePortAlpha();                          // 空操作（基线 α 固定不变）

    uint32_t GetPoolFree() const;

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
    std::vector<uint32_t>    m_portUsed;        // 每出端口总占用

    // --- 基线方案：全局固定 Alpha ---
    // 所有端口共用同一个 α，且运行期间不改变
    // 对应论文 DT（Dynamic Threshold）基线：threshold = minG + α × poolFree
    double   m_globalAlpha;     // 全局唯一 α（固定不变，= SWITCH_MMU_ALPHA = 0.5）

    bool     m_evalScheduled;   // 保留字段（接口兼容，基线中为空操作）
};

} /* namespace ns3 */

#endif /* SWITCH_MMU_H */
