#ifndef REPLAY_BUFFER_H
#define REPLAY_BUFFER_H

#include "ns3/nstime.h"
#include "ns3/ptr.h"
#include "ns3/simulator.h"
#include <vector>
#include <stdint.h>
#define MAX_SN 65536  // 假设最大序列号为 16-bit 回绕
namespace ns3 {

// 前向声明 Flit 类，假设你已经在其他地方定义了它
class Flit;

/**
 * @brief 重传缓冲区的单个条目
 * 维护 Flit 指针及其发送状态
 */
struct ReplayEntry {
    Ptr<Flit> flit;         // 数据副本
    bool isAcked;           // true: 对方已收到; false: 等待确认
    bool isRetransmitting;  // true: 正在重传队列中 (防止重复入队)
    Time lastSentTime;      // 上次物理发送的时间 (用于 RTT 冷却判断)

    ReplayEntry() 
        : flit(nullptr), 
          isAcked(true),    // 默认为 true (空闲状态视为已解决)
          isRetransmitting(false), 
          lastSentTime(Seconds(0)) {}
};

/**
 * @brief 基于环形数组的重传缓冲区
 * 用于 Link Layer 的选择重传 (SR) 协议
 */
class ReplayBuffer {
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
    void AddNewPacket(uint16_t sn, Ptr<Flit> flit);

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

    /**
     * @brief 获取 Flit 指针 (用于重传发送)
     */
    Ptr<Flit> GetFlit(uint16_t sn);

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