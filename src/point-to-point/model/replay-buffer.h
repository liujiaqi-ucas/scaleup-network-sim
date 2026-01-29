/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef REPLAY_BUFFER_H
#define REPLAY_BUFFER_H

#include <deque>
#include <stdint.h>
#include "ns3/packet.h"
#include "ns3/nstime.h"
#include "ns3/ptr.h"

namespace ns3 {

/**
 * \brief GBN 重传缓冲区单元
 */
struct ReplayItem {
    uint16_t seq_num;       // 序号
    Ptr<Packet> packet;     // 数据包副本 (COW机制，开销很小)
    Time time_sent;         // 首次发送时间 (用于计算 RTT)
    uint32_t retry_count;   // 重传计数 (用于统计或死链检测)
    ReplayItem() 
        : seq_num(0), 
          packet(nullptr), 
          time_sent(Seconds(0)), 
          retry_count(0) 
    {}
    ReplayItem(uint16_t s, Ptr<Packet> p, Time t)
        : seq_num(s), packet(p), time_sent(t), retry_count(0) {}
};

/**
 * \brief 专为 Scale-up 网络设计的 GBN 重传缓冲区
 * * 特性：
 * 1. 基于 std::deque 实现 O(1) 的头部 ACK 和尾部 Push。
 * 2. 支持通过序号计算偏移量，实现 O(1) 的 NACK 定位。
 * 3. 内置 16-bit 序号回绕处理逻辑。
 */
class ReplayBuffer 
{
public:
    ReplayBuffer ();
    virtual ~ReplayBuffer ();

    /**
     * \brief 设置窗口大小限制
     */
    void SetMaxSize (uint32_t size);
    uint32_t GetMaxSize () const;

    /**
     * \brief 当前缓冲区还有多少个包
     */
    uint32_t Size () const;

    /**
     * \brief 缓冲区是否为空
     */
    bool Empty () const;

    /**
     * \brief 缓冲区是否满了 (用于流控)
     */
    bool IsFull () const;

    /**
     * \brief 保存新发送的包
     * \param seq 序号
     * \param p 数据包
     */
    void Push (uint16_t seq, Ptr<Packet> p);

    /**
     * \brief 处理 ACK，移除已确认的包
     * \param ack_seq 确认序号 (累计确认)
     * \return 移除了多少个包
     */
    uint32_t Ack (uint16_t ack_seq);

    /**
     * \brief 根据序号获取缓冲区内的 Item (用于重传)
     * \param seq 目标序号
     * \return 指向 Item 的指针，如果找不到返回 nullptr
     */
    ReplayItem* GetItem (uint16_t seq);

    /**
     * \brief 获取队头元素的序号 (也就是最老的没确认的包)
     * \return 如果空返回 0 (需先判空)
     */
    uint16_t GetFrontSeq () const;

    /**
     * \brief 获取整个容器的迭代器 (如果需要遍历全部重传)
     */
    std::deque<ReplayItem>::iterator Begin ();
    std::deque<ReplayItem>::iterator End ();

    // --- 静态辅助函数：16位序号运算 ---
    
    // 判断 a 是否比 b 新 (a > b)
    static bool IsNewer (uint16_t a, uint16_t b);
    // 判断 a 是否小于等于 b (a <= b)
    static bool IsLeq (uint16_t a, uint16_t b);
    // 计算距离 (a - b)
    static int SeqDist (uint16_t head, uint16_t tail);

private:
    std::deque<ReplayItem> m_buffer;
    uint32_t m_max_size;

};

} // namespace ns3

#endif /* REPLAY_BUFFER_H */