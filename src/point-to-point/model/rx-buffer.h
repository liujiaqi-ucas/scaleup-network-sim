// filepath: [rx-buffer.h](http://_vscodecontentref_/3)
#pragma once
#include "ns3/packet.h"
#include "ns3/ptr.h"
#include "ns3/simple-ref-count.h"
#include <vector>
#include <stdint.h>
#include <iostream>
#include <iomanip>

#ifndef MAX_SN
#define MAX_SN 65536
#endif

namespace ns3 {

/**
 * @brief 接收端重排序缓冲区（Reorder Buffer）
 *
 * 设计：
 *  - 环形数组，大小 m_size，由外部流控保证不溢出
 *  - m_head：当前期望的最小 SN（即 rxNext/expected_seq），由 AdvanceHead() 推进
 *  - m_tail：已收到的最大 SN + 1（用于打印和调试）
 *  - 每个槽记录 storedSn，防止环形回绕时的"假命中"
 */
class RxBuffer : public SimpleRefCount<RxBuffer> {
public:
    /**
     * @param size 缓冲区容量（建议与发送窗口大小一致，由流控保证不溢出）
     */
    explicit RxBuffer(uint16_t size);
    ~RxBuffer();

    // =========================================================
    // 核心操作
    // =========================================================

    /**
     * @brief 存入一个包
     * @param sn 序列号
     * @param p  包指针
     * @return true=成功存入，false=溢出或重复（不会覆盖）
     */
    bool StorePacket(uint16_t sn, Ptr<Packet> p);

    /**
     * @brief 获取包指针（不清除槽位）
     * @param sn 序列号
     * @return 包指针，未收到则返回 nullptr
     */
    Ptr<Packet> GetPacket(uint16_t sn) const;

    /**
     * @brief 提交并清理槽位（包已转发给上层后调用）
     * 只允许清理 m_head 指向的槽，清理后自动推进 m_head
     * @return 被清理的 SN（即旧的 m_head）
     */
    uint16_t CommitHead();

    /**
     * @brief 批量提交：从 m_head 开始，连续提交所有已收到的包
     * @param outPackets 输出：按序的包指针列表
     * @return 提交的包数量
     */
    uint32_t CommitContiguous(std::vector<Ptr<Packet>>& outPackets);

    // =========================================================
    // 状态查询
    // =========================================================

    /** @brief 查询某个 SN 是否已收到 */
    bool IsReceived(uint16_t sn) const;

    /** @brief 查询 m_head 处是否已收到（可以提交） */
    bool IsHeadReady() const;

    /** @brief 获取当前头指针（最小未提交 SN） */
    uint16_t GetHead() const { return m_head; }

    /** @brief 获取当前缓存的包数量 */
    uint16_t GetCount() const { return m_count; }

    /** @brief 是否为空 */
    bool IsEmpty() const { return m_count == 0; }

    /**
     * @brief 生成 128bit NACK 位图
     * 从 baseSn + 1 开始，扫描后续 128 个槽位
     * bit=1 表示已收到，bit=0 表示缺失
     * @param baseSn    基准序列号 (FirstMissing)
     * @param bitmapHigh 输出：高 64 位 (对应 baseSn+65 ~ baseSn+128)
     * @param bitmapLow  输出：低 64 位 (对应 baseSn+1  ~ baseSn+64)
     */
    void GenerateNackBitmap(uint16_t baseSn,
                            uint64_t &bitmapHigh,
                            uint64_t &bitmapLow) const;

    // =========================================================
    // 调试
    // =========================================================

    /**
     * @brief 打印缓冲区详细状态
     * 从 m_head 开始，打印到最后一个有数据的位置
     * 空位也会打印，显示"缺失"
     */
    void PrintDebugState() const;

private:
    uint16_t GetIndex(uint16_t sn) const;

    // 每个槽的完整信息
    struct Slot {
        Ptr<Packet> pkt;        // 包指针
        uint16_t    storedSn;   // 实际存的 SN（用于防止环形假命中）
        bool        occupied;   // 是否有数据

        Slot() : pkt(nullptr), storedSn(0), occupied(false) {}
    };

    std::vector<Slot> m_buffer; // 物理存储
    uint16_t          m_size;   // 缓冲区容量
    uint16_t          m_head;   // 头指针：最小未提交 SN
    uint16_t          m_tail;   // 尾指针：max(已收到 SN) + 1，仅用于调试打印
    uint16_t          m_count;  // 当前缓存包数
};

} // namespace ns3