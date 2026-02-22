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
    m_count++;
    m_buffer[idx] = p;
    m_isReceived[idx] = true; // 【点灯】：标记为收到
}
void RxBuffer::PrintDebugState(uint16_t firstExpected, uint16_t windowSize) const {
    // 第一步：从 firstExpected 向后扫描，找到最后一个有数据的位置
    int lastFilledOffset = -1;  // -1 表示后面全是空的
    for (int i = 0; i < (int)windowSize; i++) {
        uint16_t sn = (firstExpected + i) % MAX_SN;
        uint16_t idx = GetIndex(sn);
        if (m_isReceived[idx]) {
            lastFilledOffset = i;  // 持续更新，最终是最后一个有数据的位置
        }
    }

    std::cout << "===== RxBuffer 详细状态 =====" << std::endl;
    std::cout << "  缓冲区大小: " << m_size 
              << "  当前缓存包数: " << m_count << std::endl;
    std::cout << "  扫描起点(firstExpected): " << firstExpected << std::endl;

    if (lastFilledOffset == -1) {
        std::cout << "  [缓冲区为空，firstExpected 之后没有任何数据]" << std::endl;
        std::cout << "=============================" << std::endl;
        return;
    }

    std::cout << "  扫描范围: SN " << firstExpected 
              << " ~ " << (uint16_t)((firstExpected + lastFilledOffset) % MAX_SN) 
              << "（共 " << lastFilledOffset + 1 << " 个槽位）" << std::endl;
    std::cout << "  格式：[SN] (idx=物理下标) 状态" << std::endl;
    std::cout << "  -------" << std::endl;

    // 第二步：打印从 firstExpected 到 lastFilledOffset 的每个槽位
    for (int i = 0; i <= lastFilledOffset; i++) {
        uint16_t sn = (firstExpected + i) % MAX_SN;
        uint16_t idx = GetIndex(sn);
        bool received = m_isReceived[idx];

        std::cout << "  [SN=" << std::setw(5) << sn 
                  << "] (idx=" << std::setw(3) << idx << ") ";

        if (received) {
            std::cout << "✓ 已收到";
            // 如果你想打印包的大小，可以取消下面的注释
            // if (m_buffer[idx] != nullptr) {
            //     std::cout << "  size=" << m_buffer[idx]->GetSize() << "B";
            // }
        } else {
            std::cout << "✗ 缺失  ← 需要重传";
        }
        std::cout << std::endl;
    }

    std::cout << "=============================" << std::endl;
}
Ptr<Packet> RxBuffer::GetPacket(uint16_t sn) {
    uint16_t idx = GetIndex(sn);
    return m_buffer[idx];
}
uint16_t RxBuffer::GetCount() const { return m_count; }
bool RxBuffer::IsEmpty() const { return m_count == 0; }
void RxBuffer::ClearEntry(uint16_t sn) {
    uint16_t idx = GetIndex(sn);
    
    m_buffer[idx] = nullptr;   // 释放 Packet 引用
    m_isReceived[idx] = false; // 【灭灯】：标记为空，供下一圈复用
    m_count--;
}

bool RxBuffer::IsReceived(uint16_t sn) const {
    uint16_t idx = GetIndex(sn);
    return m_isReceived[idx];
}

// 【最关键的函数】：将 vector<bool> 压缩成 uint32
void RxBuffer::GenerateNackBitmap(uint16_t baseSn,
                                  uint64_t &bitmapHigh,
                                  uint64_t &bitmapLow) const
{
    bitmapHigh = 0;
    bitmapLow  = 0;

    for (int i = 0; i < 128; i++) {
        uint16_t targetSn = (uint16_t)(baseSn + 1 + i);
        if (IsReceived(targetSn)) {
            if (i < 64)
                bitmapLow  |= (1ULL << i);
            else
                bitmapHigh |= (1ULL << (i - 64));
        }
    }
}

} // namespace ns3