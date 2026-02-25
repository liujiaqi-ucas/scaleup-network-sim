#include "sr-replaybuffer.h"
#include "ns3/log.h"
#include <iostream> // 用于 std::cout
#include <iomanip>
namespace ns3 {

NS_LOG_COMPONENT_DEFINE("ReplayBuffer");

// 构造函数：初始化重传缓冲区
ReplayBuffer::ReplayBuffer(uint16_t size)
    : m_size(size)
{
    // 设置 vector 的大小
    m_buffer.resize(m_size);
    NS_LOG_INFO("ReplayBuffer created with size " << m_size);
}

// 析构函数：清理资源
ReplayBuffer::~ReplayBuffer() 
{
    m_buffer.clear();
}

// 辅助函数：根据序列号计算在 vector 中的索引
uint16_t 
ReplayBuffer::GetIndex(uint16_t sn) const 
{
    // 使用取模运算实现环形缓冲区映射
    return sn % m_size;
}

// 核心函数：添加新包到重传缓冲区
void 
ReplayBuffer::AddNewPacket(uint16_t sn, Ptr<Packet> flit) 
{
    uint16_t idx = GetIndex(sn);

    // 安全检查：理论上我们要写入的位置应该是空闲的 (isAcked=true)
    // 如果流控机制工作正常，这里不应该覆盖尚未确认的数据
    if (!m_buffer[idx].isAcked) {
        NS_LOG_WARN("警告：正在覆盖 SN " << sn << " 的未确认数据！流控机制可能已损坏。");
    }

    // 重置该条目的状态
    m_buffer[idx].flit = flit;              // 存入数据包（副本）
    m_buffer[idx].sn = sn;
    m_buffer[idx].isAcked = false;          // 标记为“等待确认”
    m_buffer[idx].isRetransmitting = false; // 初始状态：不在重传队列中
    m_buffer[idx].lastSentTime = Simulator::Now(); // 记录当前的发送时间
    m_buffer[idx].retxCount = 0; // <--- 【新增】新包入队，重传计数归零
    
    // 【调试打印】：入队时打印 SN 和 Vector 下标
    //std::cout << "ReplayBuffer [入队]: 存入 SN=" << sn << " 到 Vector 下标=" << idx << std::endl;

    NS_LOG_DEBUG("Added new flit SN " << sn << " at index " << idx);
}

// 核心函数：标记某个包已被确认 (ACK)
void 
ReplayBuffer::MarkAcked(uint16_t sn) 
{
    uint16_t idx = GetIndex(sn);
    // ← 新增：SN 校验，防止误确认不同 SN 映射到同一 idx 的包
    if (m_buffer[idx].sn != sn) {
        NS_LOG_WARN("MarkAcked: SN mismatch! Expected " << sn 
                     << " but slot has " << m_buffer[idx].sn);
        return;
    }
    if (m_buffer[idx].isAcked) {
        return; // 如果已经确认过了，直接返回
    }

    m_buffer[idx].isAcked = true;
    // 注意：我们这里不立即删除 flit 指针 (m_buffer[idx].flit = nullptr)
    // 而是等到 FreeSlots (滑动窗口移动) 时再删。
    // 在 SR 协议中，只要 isAcked=true，它就不会被重传了。
    
    NS_LOG_DEBUG("Marked SN " << sn << " as ACKED");
}

// 核心函数：滑动窗口，清理旧数据
void 
ReplayBuffer::FreeSlots(uint16_t oldUna, uint16_t newTxUna) 
{
    // 计算需要清理的区间 [oldUna, newTxUna)
    // 这里循环清理从 oldUna 开始，直到 newTxUna 之前的所有包
    
    uint16_t current = oldUna;
    
    // 假设 MAX_SN 是外部定义的 (例如 65536)，用于处理序号回绕
    while (current != newTxUna) {
        uint16_t idx = GetIndex(current);
        
        // 【调试打印】：出队/清理时打印
        // 只有当这里原本存有数据时才打印，避免打印空槽位
        if (m_buffer[idx].flit != nullptr) {
            //std::cout << "ReplayBuffer [清理]: 清空 Vector 下标=" << idx << " (原 SN=" << current << ")" << std::endl;
        }

         // 【新增】完整清理所有字段
        m_buffer[idx].flit = nullptr;
        m_buffer[idx].sn = UINT16_MAX;           // ← 新增
        m_buffer[idx].isAcked = true;
        m_buffer[idx].isRetransmitting = false;   // ← 新增
        m_buffer[idx].retxCount = 0;              // ← 新增
        m_buffer[idx].lastSentTime = Seconds(0);  // ← 新增
        std::cout<<"清理了SN="<<current<<"所在的槽位,idx="<<idx<<std::endl;
        // 处理序号回绕 (Wrap-around)
        current = (current + 1) % MAX_SN;
    }
}

// 获取包指针（用于重传）
Ptr<Packet> 
ReplayBuffer::GetFlit(uint16_t sn) 
{
   uint16_t idx = GetIndex(sn);
    // ← 新增：SN 校验
    if (m_buffer[idx].sn != sn) {
        NS_LOG_WARN("GetFlit: SN mismatch! slot has " << m_buffer[idx].sn 
                     << " but requested " << sn);
        return nullptr;
    }
    return m_buffer[idx].flit;
}

// 查询状态：是否已确认
bool 
ReplayBuffer::IsAcked(uint16_t sn) 
{
     uint16_t idx = GetIndex(sn);
    // ← 新增：如果 SN 不匹配，说明这个槽位已经被其他 SN 覆盖了
    // 原来的包已经被 FreeSlots 清理，等价于"已确认"
    if (m_buffer[idx].sn != sn) {
        return true;
    }
    return m_buffer[idx].isAcked;
}

// 查询状态：是否正在重传中
bool 
ReplayBuffer::IsRetransmitting(uint16_t sn) 
{
    uint16_t idx = GetIndex(sn);
     // ← 新增：SN 不匹配时，槽位属于其他包，当前 SN 不在重传中
    if (m_buffer[idx].sn != sn) {
        return false;
    }
    return m_buffer[GetIndex(sn)].isRetransmitting;
}

// 设置状态：是否正在重传中
void 
ReplayBuffer::SetRetransmitting(uint16_t sn, bool val) 
{
    uint16_t idx = GetIndex(sn);
    // ← 新增：SN 校验
    if (m_buffer[idx].sn != sn) {
        NS_LOG_WARN("SetRetransmitting: SN mismatch! slot has " << m_buffer[idx].sn 
                     << " but requested " << sn);
        return;
    }
    m_buffer[idx].isRetransmitting = val;
}

// 获取上次发送时间
Time 
ReplayBuffer::GetLastSentTime(uint16_t sn) 
{
     uint16_t idx = GetIndex(sn);
    // ← 新增：SN 不匹配时返回远古时间，不会触发冷却
    if (m_buffer[idx].sn != sn) {
        return Seconds(0);
    }
    return m_buffer[idx].lastSentTime;
}

// 更新上次发送时间
void 
ReplayBuffer::UpdateLastSentTime(uint16_t sn) 
{
   uint16_t idx = GetIndex(sn);
   // ← 新增：SN 校验
    if (m_buffer[idx].sn != sn) {
        NS_LOG_WARN("UpdateLastSentTime: SN mismatch!");
        return;
    }
    m_buffer[idx].lastSentTime = Simulator::Now();
    m_buffer[idx].retxCount++; // <--- 【新增】每次更新时间（意味着重传了一次），计数+1
}
uint8_t 
ReplayBuffer::GetRetxCount(uint16_t sn) 
{
     uint16_t idx = GetIndex(sn);
    // ← 新增：SN 不匹配返回 0
    if (m_buffer[idx].sn != sn) {
        return 0;
    }
    return m_buffer[idx].retxCount;
}
void 
ReplayBuffer::PrintBuffer(uint32_t nodeId, uint32_t ifIndex) const
{
    std::cout << "========== ReplayBuffer 状态 [Node=" << nodeId 
              << " Dev=" << ifIndex 
              << " Time=" << Simulator::Now().GetNanoSeconds() << "ns"
              << " Size=" << m_size << "] ==========" << std::endl;

    uint16_t occupied = 0;
    uint16_t acked = 0;
    uint16_t unacked = 0;
    uint16_t retransmitting = 0;

    std::cout << std::left
              << std::setw(8)  << "Idx"
              << std::setw(10) << "SN"
              << std::setw(10) << "Acked"
              << std::setw(10) << "Retxing"
              << std::setw(8)  << "RetxCnt"
              << std::setw(20) << "LastSentTime(ns)"
              << std::setw(10) << "HasFlit"
              << std::endl;

    std::cout << std::string(76, '-') << std::endl;

    for (uint16_t i = 0; i < m_size; ++i) {
        const ReplayEntry& entry = m_buffer[i];

        // 只打印非空槽位（SN 有效 或 flit 非空 或 未确认）
        if (entry.sn == UINT16_MAX && entry.flit == nullptr && entry.isAcked) {
            continue;  // 空槽位，跳过
        }

        occupied++;
        if (entry.isAcked) acked++;
        else unacked++;
        if (entry.isRetransmitting) retransmitting++;

        std::cout << std::left
                  << std::setw(8)  << i
                  << std::setw(10) << (entry.sn == UINT16_MAX ? "INVALID" : std::to_string(entry.sn))
                  << std::setw(10) << (entry.isAcked ? "Y" : "N")
                  << std::setw(10) << (entry.isRetransmitting ? "Y" : "N")
                  << std::setw(8)  << (int)entry.retxCount
                  << std::setw(20) << entry.lastSentTime.GetNanoSeconds()
                  << std::setw(10) << (entry.flit != nullptr ? "Y" : "N")
                  << std::endl;
    }

    std::cout << std::string(76, '-') << std::endl;
    std::cout << "摘要: 占用=" << occupied 
              << " 已确认=" << acked 
              << " 未确认=" << unacked 
              << " 重传中=" << retransmitting 
              << " 空闲=" << (m_size - occupied) 
              << std::endl;
    std::cout << "==========================================" << std::endl;
}
} // namespace ns3