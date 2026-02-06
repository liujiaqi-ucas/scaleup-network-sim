#include "rx-buffer.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE("RxBuffer");

namespace ns3 {

RxBuffer::RxBuffer(uint16_t size) 
    : m_size(size)
{
    // 初始化数组
    m_buffer.resize(size);
    m_isReceived.resize(size, false); // 初始全空
    NS_LOG_INFO("RxBuffer created with size " << size);
}

RxBuffer::~RxBuffer() {
    m_buffer.clear();
    m_isReceived.clear();
}

uint16_t RxBuffer::GetIndex(uint16_t sn) const {
    // 简单的取模映射
    return sn % m_size;
}

void RxBuffer::StorePacket(uint16_t sn, Ptr<Packet> p) {
    uint16_t idx = GetIndex(sn);
    
    // 安全检查：如果这个位置已经是 true，说明发生了逻辑错误或重复接收
    if (m_isReceived[idx]) {
        NS_LOG_WARN("Overwriting existing packet at SN " << sn);
    }

    m_buffer[idx] = p;
    m_isReceived[idx] = true; // 【点灯】：标记为收到
}

Ptr<Packet> RxBuffer::GetPacket(uint16_t sn) {
    uint16_t idx = GetIndex(sn);
    return m_buffer[idx];
}

void RxBuffer::ClearEntry(uint16_t sn) {
    uint16_t idx = GetIndex(sn);
    
    m_buffer[idx] = nullptr;   // 释放 Packet 引用
    m_isReceived[idx] = false; // 【灭灯】：标记为空，供下一圈复用
}

bool RxBuffer::IsReceived(uint16_t sn) const {
    uint16_t idx = GetIndex(sn);
    return m_isReceived[idx];
}

// 【最关键的函数】：将 vector<bool> 压缩成 uint32
uint32_t RxBuffer::GenerateNackBitmap(uint16_t baseSn) const {
    uint32_t bitmap = 0;

    // 我们只关心 baseSn (FirstMissing) 后面紧着的 32 个包
    for (int i = 0; i < 32; i++) {
        // 计算目标 SN (注意：这里不需要处理 MAX_SN 回绕，直接加即可)
        // 因为我们在 GetIndex 里会取模
        // Bit 0 对应 baseSn + 1
        uint16_t targetSn = baseSn + 1 + i;
        
        uint16_t idx = GetIndex(targetSn);

        // 如果本地收到了
        if (m_isReceived[idx]) {
            // 将 bitmap 的第 i 位置 1
            bitmap |= (1 << i);
        }
    }
    
    return bitmap;
}

} // namespace ns3