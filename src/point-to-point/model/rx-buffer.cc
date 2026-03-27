// filepath: [rx-buffer.cc](http://_vscodecontentref_/4)
#include "rx-buffer.h"
#include "ns3/log.h"

NS_LOG_COMPONENT_DEFINE("RxBuffer");

namespace ns3 {

// =========================================================
// 构造 / 析构
// =========================================================

RxBuffer::RxBuffer(uint16_t size)
    : m_size(size), m_head(0), m_tail(0), m_count(0)
{
    m_buffer.resize(m_size);
    NS_LOG_INFO("RxBuffer created, size=" << m_size);
}

RxBuffer::~RxBuffer()
{
    m_buffer.clear();
}

// =========================================================
// 辅助
// =========================================================

uint16_t RxBuffer::GetIndex(uint16_t sn) const
{
    return sn % m_size;
}

// =========================================================
// 核心操作
// =========================================================

bool RxBuffer::StorePacket(uint16_t sn, Ptr<Packet> p)
{
    // ---- 1. 溢出检查：sn 必须在窗口 [m_head, m_head + m_size) 内 ----
    int distFromHead = (int)(sn - m_head);
    if (distFromHead < 0 || distFromHead >= (int)m_size) {
        return false;
    }

    uint16_t idx = GetIndex(sn);

    // ---- 2. 重复检查 ----
    if (m_buffer[idx].occupied && m_buffer[idx].storedSn == sn) {
        return false;
    }

    // ---- 3. 槽位冲突检查（流控失效时的保护） ----
    if (m_buffer[idx].occupied && m_buffer[idx].storedSn != sn) {
        std::cout << "StorePacket: 槽位冲突！idx=" << idx
                     << " 已有 SN=" << m_buffer[idx].storedSn
                     << " 但试图写入 SN=" << sn
                     << " —— 流控可能失效" << std::endl;
        return false;
    }

    // ---- 4. 存入 ----
    m_buffer[idx].pkt      = p;
    m_buffer[idx].storedSn = sn;
    m_buffer[idx].occupied = true;
    m_count++;

    // ---- 5. 更新尾指针（保持 m_tail = max已收SN + 1） ----
    int distFromTail = (int)(sn - m_tail);
    if (distFromTail >= 0) {
        m_tail = (uint16_t)(sn + 1);
    }

    return true;
}

// ---------------------------------------------------------

Ptr<Packet> RxBuffer::GetPacket(uint16_t sn) const
{
    uint16_t idx = GetIndex(sn);
    if (!m_buffer[idx].occupied || m_buffer[idx].storedSn != sn) {
        return nullptr;
    }
    return m_buffer[idx].pkt;
}

// ---------------------------------------------------------

uint16_t RxBuffer::CommitHead()
{
    uint16_t idx = GetIndex(m_head);

    // 必须保证 head 处已有数据才能提交
    NS_ASSERT_MSG(m_buffer[idx].occupied && m_buffer[idx].storedSn == m_head,
                  "CommitHead: m_head=" << m_head << " 处没有数据，不能提交");

    uint16_t committed = m_head;

    // 清理槽位
    m_buffer[idx].pkt      = nullptr;
    m_buffer[idx].occupied = false;

    m_count--;
    m_head = (uint16_t)(m_head + 1);

    return committed;
}

// ---------------------------------------------------------

uint32_t RxBuffer::CommitContiguous(std::vector<Ptr<Packet>>& outPackets)
{
    uint32_t n = 0;
    while (IsHeadReady()) {
        uint16_t idx = GetIndex(m_head);
        outPackets.push_back(m_buffer[idx].pkt);

        // 清理
        m_buffer[idx].pkt      = nullptr;
        m_buffer[idx].occupied = false;
        m_count--;
        m_head = (uint16_t)(m_head + 1);
        n++;
    }
    return n;
}

// =========================================================
// 状态查询
// =========================================================

bool RxBuffer::IsReceived(uint16_t sn) const
{
    uint16_t idx = GetIndex(sn);
    return m_buffer[idx].occupied && m_buffer[idx].storedSn == sn;
}

bool RxBuffer::IsHeadReady() const
{
    return IsReceived(m_head);
}

// ---------------------------------------------------------

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

// =========================================================
// 调试打印
// =========================================================

void RxBuffer::PrintDebugState() const
{
    std::cout << "===== RxBuffer 状态 =====" << std::endl;
    std::cout << "  容量=" << m_size
              << "  已缓存=" << m_count
              << "  head(期望SN)=" << m_head
              << "  tail=" << m_tail
              << std::endl;

    if (m_head == m_tail || m_count == 0) {
        std::cout << "  [缓冲区为空]" << std::endl;
        std::cout << "=========================" << std::endl;
        return;
    }

    uint16_t span = (uint16_t)(m_tail - m_head);
    if (span > m_size) {
        span = m_size;
    }

    std::cout << "  [head=" << m_head << " ... tail-1="
              << (uint16_t)(m_tail - 1) << "]" << std::endl;
    std::cout << "  SN      idx  状态" << std::endl;
    std::cout << "  ------  ---  ----" << std::endl;

    for (uint16_t i = 0; i < span; i++) {
        uint16_t sn  = (uint16_t)(m_head + i);
        uint16_t idx = GetIndex(sn);
        bool     ok  = m_buffer[idx].occupied && m_buffer[idx].storedSn == sn;

        std::cout << "  "
                  << std::setw(6) << sn
                  << "  "
                  << std::setw(3) << idx
                  << "  ";

        if (ok) {
            std::cout << "✓  已收到";
            if (m_buffer[idx].pkt != nullptr) {
                std::cout << "  (" << m_buffer[idx].pkt->GetSize() << " B)";
            }
        } else {
            std::cout << "✗  缺失 ← 待重传";
        }

        if (sn == m_head)            std::cout << "  ← HEAD";
        if (sn == (uint16_t)(m_tail - 1)) std::cout << "  ← TAIL(最后收到)";

        std::cout << std::endl;
    }

    std::cout << "=========================" << std::endl;
}

} // namespace ns3
