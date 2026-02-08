#include "sr-replaybuffer.h"
#include "ns3/log.h"


namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ReplayBuffer");

ReplayBuffer::ReplayBuffer(uint16_t size)
    : m_size(size)
{
    // 初始化 vector 大小
    m_buffer.resize(m_size);
    NS_LOG_INFO("ReplayBuffer created with size " << m_size);
}

ReplayBuffer::~ReplayBuffer() 
{
    m_buffer.clear();
}

uint16_t 
ReplayBuffer::GetIndex(uint16_t sn) const 
{
    // 简单的取模运算实现环形映射
    return sn % m_size;
}

void 
ReplayBuffer::AddNewPacket(uint16_t sn, Ptr<Packet> flit) 
{
    uint16_t idx = GetIndex(sn);

    // 安全检查：理论上这个位置应该是空闲的 (isAcked=true)
    // 如果 Controller 的流控做得好，这里不会覆盖未确认的数据
    if (!m_buffer[idx].isAcked) {
        NS_LOG_WARN("Overwriting un-acked data at SN " << sn << "! Flow control might be broken.");
    }

    // 重置条目状态
    m_buffer[idx].flit = flit;
    m_buffer[idx].isAcked = false;            // 标记为等待确认
    m_buffer[idx].isRetransmitting = false;   // 初始状态不在重传队列
    m_buffer[idx].lastSentTime = Simulator::Now(); // 记录初次发送时间
    
    NS_LOG_DEBUG("Added new flit SN " << sn << " at index " << idx);
}

void 
ReplayBuffer::MarkAcked(uint16_t sn) 
{
    uint16_t idx = GetIndex(sn);
    
    if (m_buffer[idx].isAcked) {
        return; // 已经确认过了，无需操作
    }

    m_buffer[idx].isAcked = true;
    // 注意：我们这里不删除 flit 指针 (m_buffer[idx].flit = nullptr)
    // 而是等到 FreeSlots (窗口滑动) 时再删，或者为了调试保留引用。
    // 在 SR 协议中，isAcked=true 就足够阻止它被重传了。
    
    NS_LOG_DEBUG("Marked SN " << sn << " as ACKED");
}

void 
ReplayBuffer::FreeSlots(uint16_t oldUna, uint16_t newTxUna) 
{
    // 计算需要清理的区间
    // 注意：这里的循环条件需要处理序列号回绕，但因为我们是操作 buffer
    // 只要 newTxUna 确实是比 oldUna 靠后的（Controller 保证），
    // 我们直接按逻辑距离循环即可。
    
    // 这里为了简化，假设 Controller 调用时 oldUna 和 newTxUna 的距离是正常的
    // 更好的做法是 Controller 告诉我们 "释放哪个 SN"
    
    uint16_t current = oldUna;
    // 假设 MAX_SN 是外部定义的，这里用简单的 != 来遍历
    // 实际使用建议传入 distance 避免回绕死循环
    while (current != newTxUna) {
        uint16_t idx = GetIndex(current);
        
        // 清理内存
        m_buffer[idx].flit = nullptr; 
        m_buffer[idx].isAcked = true; // 恢复为默认安全状态
        
       // 【建议改为这样】
        // 无论 maxSn 是 4096 还是 65536，这行代码都是对的
        // 注意：如果是 65536，这里需要用 uint32_t 强转一下防止计算溢出后再取模
        current = (current + 1) % MAX_SN;
    }
}

Ptr<Packet> 
ReplayBuffer::GetFlit(uint16_t sn) 
{
    uint16_t idx = GetIndex(sn);
    return m_buffer[idx].flit;
}

bool 
ReplayBuffer::IsAcked(uint16_t sn) 
{
    return m_buffer[GetIndex(sn)].isAcked;
}

bool 
ReplayBuffer::IsRetransmitting(uint16_t sn) 
{
    return m_buffer[GetIndex(sn)].isRetransmitting;
}

void 
ReplayBuffer::SetRetransmitting(uint16_t sn, bool val) 
{
    m_buffer[GetIndex(sn)].isRetransmitting = val;
}

Time 
ReplayBuffer::GetLastSentTime(uint16_t sn) 
{
    return m_buffer[GetIndex(sn)].lastSentTime;
}

void 
ReplayBuffer::UpdateLastSentTime(uint16_t sn) 
{
    m_buffer[GetIndex(sn)].lastSentTime = Simulator::Now();
}

} // namespace ns3