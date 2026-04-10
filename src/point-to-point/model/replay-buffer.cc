#include "replay-buffer.h"

namespace ns3 {

ReplayBuffer::ReplayBuffer(uint32_t capacity)
    : m_capacity(capacity), m_freeCount(capacity)
{
    m_slots.resize(capacity, nullptr);
    for (uint32_t i = 0; i < capacity; i++) {
        m_freeList.push(i);
    }
}

bool ReplayBuffer::HasSpace() const {
    return m_freeCount > 0;
}

int ReplayBuffer::Allocate() {
    if (m_freeList.empty()) return -1;
    int idx = m_freeList.front();
    m_freeList.pop();
    m_freeCount--;
    return idx;
}

void ReplayBuffer::Store(int index, Ptr<Packet> p) {
    m_slots[index] = p;
}

Ptr<Packet> ReplayBuffer::Read(int index) const {
    if (index < 0 || index >= (int)m_capacity) return nullptr;
    return m_slots[index];
}

void ReplayBuffer::Free(int index) {
    if (index < 0 || index >= (int)m_capacity) return;
    m_slots[index] = nullptr;
    m_freeList.push(index);
    m_freeCount++;
}

} // namespace ns3
