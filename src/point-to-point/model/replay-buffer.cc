/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "replay-buffer.h"
#include "ns3/log.h"
#include "ns3/simulator.h"

NS_LOG_COMPONENT_DEFINE ("ReplayBuffer");

namespace ns3 {

// ===========================================================================
// 构造与析构
// ===========================================================================

ReplayBuffer::ReplayBuffer ()
    : m_max_size (512) // 默认窗口 512，这个是参数
{
}

ReplayBuffer::~ReplayBuffer ()
{
    m_buffer.clear ();
}

void 
ReplayBuffer::SetMaxSize (uint32_t size)
{
    m_max_size = size;
}

uint32_t 
ReplayBuffer::GetMaxSize () const
{
    return m_max_size;
}

uint32_t 
ReplayBuffer::Size () const
{
    return (uint32_t)m_buffer.size ();
}

bool 
ReplayBuffer::Empty () const
{
    return m_buffer.empty ();
}

bool 
ReplayBuffer::IsFull () const
{
    return m_buffer.size () >= m_max_size;
}

// ===========================================================================
// 核心操作：Push (入队)
// ===========================================================================

void 
ReplayBuffer::Push (uint16_t seq, Ptr<Packet> p)
{
    if (IsFull()) {
        NS_LOG_WARN ("ReplayBuffer is full! Pushing anyway, this implies Flow Control Bug.");
    }
    
    // 存入副本，并记录当前仿真时间
    // 注意：p->Copy() 是浅拷贝(COW)，效率很高
    m_buffer.emplace_back (seq, p->Copy (), Simulator::Now ());
}

// ===========================================================================
// 核心操作：Ack (滑动窗口)
// ===========================================================================

uint32_t 
ReplayBuffer::Ack (uint16_t ack_seq)
{
    uint32_t removed_count = 0;

    // GBN 累计确认逻辑：
    // 只要队头元素的序号 <= ack_seq (考虑回绕)，就移除
    while (!m_buffer.empty ()) 
    {
        uint16_t head_seq = m_buffer.front ().seq_num;

        // 使用 IsLeq 处理回绕比较
        if (IsLeq (head_seq, ack_seq)) 
        {
            m_buffer.pop_front (); // O(1) 操作
            removed_count++;
        } 
        else 
        {
            // 队头比 ack_seq 还新，说明还没确认到这里，停止
            break;
        }
    }
    
    return removed_count;
}

// ===========================================================================
// 核心操作：GetItem (O(1) 随机访问)
// ===========================================================================

ReplayItem* ReplayBuffer::GetItem (uint16_t seq)
{
    if (m_buffer.empty ()) {
        return nullptr;
    }

    uint16_t head_seq = m_buffer.front ().seq_num;

    // 计算 seq 相对于队头 head_seq 的偏移量
    // 例如：head=100, seq=105 -> offset=5
    // 例如：head=65530, seq=2 -> offset=8 (回绕情况)
    int offset = SeqDist (seq, head_seq);

    // 安全检查：偏移量必须在当前 buffer 范围内
    if (offset >= 0 && offset < (int)m_buffer.size ()) 
    {
        // 再次确认序号匹配 (双重保险)
        if (m_buffer[offset].seq_num == seq) {
            return &m_buffer[offset];
        }
    }

    return nullptr; // 没找到 (可能已经被 ACK 了，或者还没发)
}

uint16_t 
ReplayBuffer::GetFrontSeq () const
{
    if (m_buffer.empty ()) return 0;
    return m_buffer.front ().seq_num;
}

std::deque<ReplayItem>::iterator 
ReplayBuffer::Begin ()
{
    return m_buffer.begin ();
}

std::deque<ReplayItem>::iterator 
ReplayBuffer::End ()
{
    return m_buffer.end ();
}

// ===========================================================================
// 静态辅助函数：16-bit 序号数学 (Sequence Number Arithmetic)
// ===========================================================================

// 距离定义：在环形空间上，a 在 b 前面多少步？
// 范围：[-32768, 32767]
int 
ReplayBuffer::SeqDist (uint16_t a, uint16_t b)
{
    // 利用 int16_t 的强制转换处理补码，自动搞定回绕
    // 比如 (2 - 65535) -> 3
    return (int16_t)(a - b);
}

bool 
ReplayBuffer::IsNewer (uint16_t a, uint16_t b)
{
    // 如果距离 > 0，说明 a 比 b 新
    return SeqDist (a, b) > 0;
}

bool 
ReplayBuffer::IsLeq (uint16_t a, uint16_t b)
{
    // 如果距离 <= 0，说明 a 比 b 旧，或者相等
    return SeqDist (a, b) <= 0;
}

} // namespace ns3