#ifndef REPLAY_BUFFER_H
#define REPLAY_BUFFER_H

#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"
#include "ns3/packet.h"
#include <vector>
#include <stdint.h>
#define MAX_SN 65536  // 假设最大序列号为 16-bit 回绕
namespace ns3 {



/**
 * @brief 重传缓冲区的单个条目
 * 维护 Flit 指针及其发送状态
 */
struct ReplayEntry {
    Ptr<Packet> flit;
    uint16_t sn;
    bool isAcked;//该包是否已经被确认
    bool isRetransmitting;//该包现在是否在重传队列里面
    Time lastSentTime;//上次发送时间
    uint8_t retxCount; // <--- 【新增】记录重传次数

    ReplayEntry() 
        : flit(nullptr), 
        sn(UINT16_MAX),
          isAcked(true), 
          isRetransmitting(false), 
          lastSentTime(Seconds(0)),
          retxCount(0) {} // <--- 【新增】初始化为0
};

/**
 * @brief 基于环形数组的重传缓冲区
 * 用于 Link Layer 的选择重传 (SR) 协议
 */
class ReplayBuffer : public SimpleRefCount<ReplayBuffer> {
public:
    /**
     * @param size 缓冲区大小 (例如 128)
     */
    ReplayBuffer(uint16_t size);
    ~ReplayBuffer();

    /**
     * @brief 存入一个新的 Flit (由 TxNext 推进时调用)
     * @param sn 序列号
     * @param flit Flit 指针
     */
    void AddNewPacket(uint16_t sn, Ptr<Packet> flit);

    /**
     * @brief 标记某个 SN 为已确认 (ACKED)
     * @param sn 序列号
     */
    void MarkAcked(uint16_t sn);

    /**
     * @brief 释放/清理掉小于 newTxUna 的旧数据 (滑动窗口)
     * @param oldUna 旧的 TxUna
     * @param newTxUna 新的 TxUna
     */
    void FreeSlots(uint16_t oldUna, uint16_t newTxUna);

    // ============ 状态查询与获取 ============
    uint8_t GetRetxCount(uint16_t sn);
    /**
     * @brief 获取 Flit 指针 (用于重传发送)
     */
    Ptr<Packet> GetFlit(uint16_t sn);

    /**
     * @brief 查询是否已确认
     */
    bool IsAcked(uint16_t sn);

    /**
     * @brief 查询是否正在重传中
     */
    bool IsRetransmitting(uint16_t sn);

    /**
     * @brief 设置重传状态
     * @param val true 表示入队了, false 表示刚发完
     */
    void SetRetransmitting(uint16_t sn, bool val);

    /**
     * @brief 获取上次发送时间
     */
    Time GetLastSentTime(uint16_t sn);

    /**
     * @brief 更新上次发送时间为当前时间 (Simulator::Now)
     */
    void UpdateLastSentTime(uint16_t sn);
    /**
     * @brief 打印重传缓冲区中所有非空槽位的状态（调试用）
     * @param nodeId   节点 ID（方便区分不同节点的输出）
     * @param ifIndex  端口编号
     */
    void PrintBuffer(uint32_t nodeId, uint32_t ifIndex) const;

private:
    /**
     * @brief 计算环形数组索引
     */
    uint16_t GetIndex(uint16_t sn) const;

    std::vector<ReplayEntry> m_buffer; // 物理存储
    uint16_t m_size;                   // 缓冲区容量
};

} // namespace ns3

#endif // REPLAY_BUFFER_H