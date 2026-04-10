#ifndef REPLAY_BUFFER_H
#define REPLAY_BUFFER_H

#include "ns3/packet.h"
#include <vector>
#include <queue>

namespace ns3 {

/**
 * \brief 固定大小的 per-port 重传重放缓冲区
 *
 * 传统设计：flit 发送后从转发池释放，拷贝到此独立缓冲区。
 * ACK 确认后释放。重传时从此缓冲区读取。
 */
class ReplayBuffer {
public:
    ReplayBuffer(uint32_t capacity);

    bool HasSpace() const;          // 是否有空闲槽位
    int Allocate();                  // 分配一个槽位，返回下标，满返回 -1
    void Store(int index, Ptr<Packet> p);
    Ptr<Packet> Read(int index) const;
    void Free(int index);

    uint32_t GetCapacity() const { return m_capacity; }
    uint32_t GetUsed() const { return m_capacity - m_freeCount; }

private:
    uint32_t m_capacity;
    uint32_t m_freeCount;
    std::vector<Ptr<Packet>> m_slots;
    std::queue<int> m_freeList;
};

} // namespace ns3

#endif // REPLAY_BUFFER_H
