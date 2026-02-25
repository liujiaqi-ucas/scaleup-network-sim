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
    // 用有符号距离判断：dist > 0 表示 sn 在 m_head 之后
    int distFromHead = (int)(sn - m_head);
    if (distFromHead < 0 || distFromHead >= (int)m_size) {
        //std::cout << "StorePacket: SN=" << sn
                    //<< " 超出窗口 [" << m_head
                    //<< ", " << (uint16_t)(m_head + m_size) << ")，丢弃" << std::endl;
        return false;
    }

    uint16_t idx = GetIndex(sn);

    // ---- 2. 重复检查 ----
    if (m_buffer[idx].occupied && m_buffer[idx].storedSn == sn) {
        //std::cout << "StorePacket: SN=" << sn << " 重复收到，忽略" << std::endl;
        return false;
    }

    // ---- 3. 槽位冲突检查（流控失效时的保护） ----
    if (m_buffer[idx].occupied && m_buffer[idx].storedSn != sn) {
        //std::cout << "StorePacket: 槽位冲突！idx=" << idx
                     //<< " 已有 SN=" << m_buffer[idx].storedSn
                     //<< " 但试图写入 SN=" << sn
                     //<< " —— 流控可能失效" << std::endl;
        return false;
    }

    // ---- 4. 存入 ----
    m_buffer[idx].pkt      = p;
    m_buffer[idx].storedSn = sn;
    m_buffer[idx].occupied = true;
    m_count++;

    // ---- 5. 更新尾指针（保持 m_tail = max已收SN + 1） ----
    // 如果新 sn 比当前 m_tail 更靠后，推进 m_tail
    int distFromTail = (int)(sn - m_tail);
    // std::cout << "  [TAIL更新检查] sn=" << sn 
              //<< " m_tail(更新前)=" << m_tail 
             // << " distFromTail=" << distFromTail;
    if (distFromTail >= 0) {
        m_tail = (uint16_t)(sn + 1);
        //std::cout << " → m_tail(更新后)=" << m_tail;
    } else {
        //std::cout << " → 未更新(distFromTail<0)";
    }

    //std::cout << "StorePacket: SN=" << sn << " idx=" << idx
              //<< " count=" << m_count << std::endl;
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
    // storedSn 不清零，留着也无妨（occupied=false 已保护）

    m_count--;
    m_head = (uint16_t)(m_head + 1); // 推进头指针（自动回绕）

    //std::cout << "CommitHead: committed SN=" << committed
              //<< " new head=" << m_head << std::endl;
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
    // 必须同时检查 occupied 和 storedSn，防止环形假命中
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

    // 如果 head == tail，说明缓冲区里没有任何数据
    if (m_head == m_tail || m_count == 0) {
        std::cout << "  [缓冲区为空]" << std::endl;
        std::cout << "=========================" << std::endl;
        return;
    }

    // 从 head 打印到 tail-1
    // 计算跨度，防止 tail < head（正常不会发生，但加个保护）
    uint16_t span = (uint16_t)(m_tail - m_head);
    if (span > m_size) {
        // 不应发生，但保护一下
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

        // 标注 head 和 tail
        if (sn == m_head)            std::cout << "  ← HEAD";
        if (sn == (uint16_t)(m_tail - 1)) std::cout << "  ← TAIL(最后收到)";

        std::cout << std::endl;
    }

    std::cout << "=========================" << std::endl;
}

} // namespace ns3