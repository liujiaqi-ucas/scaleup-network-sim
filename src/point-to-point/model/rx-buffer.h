#include "ns3/packet.h"
#include "ns3/ptr.h"
#include <vector>
#include <stdint.h>

namespace ns3 {

/**
 * @brief 接收端的重排序缓冲区 (Reordering Buffer)
 * 职责：
 * 1. 暂存乱序到达的包
 * 2. 维护本地接收状态 (Local Bitmap)
 * 3. 生成用于发送 NACK 的压缩位图
 */
class RxBuffer : public SimpleRefCount<RxBuffer> {
public:
    /**
     * @param size 缓冲区大小 (例如 128)
     */
    RxBuffer(uint16_t size);
    ~RxBuffer();
    // ... 其他函数 ...
    uint16_t GetCount() const; // 返回当前缓存的包数量
    bool IsEmpty() const;      // 返回是否为空
    // ... 其他成员 ...
    uint16_t m_count = 0;      // 实时计数器
    // ============ 核心操作 ============

    /**
     * @brief 存入一个包 (不管顺序还是乱序)
     * 会自动标记 isReceived = true
     */
    void StorePacket(uint16_t sn, Ptr<Packet> p);

    /**
     * @brief 获取包指针 (用于提交给上层)
     */
    Ptr<Packet> GetPacket(uint16_t sn);

    /**
     * @brief 清理槽位 (提交完成后调用)
     * 会重置 isReceived = false，释放 Packet 内存
     */
    void ClearEntry(uint16_t sn);

    // ============ 状态查询 ============

    /**
     * @brief 查询某个 SN 是否已经收到
     */
    bool IsReceived(uint16_t sn) const;

    /**
     * @brief 生成 NACK 位图 (关键函数)
     * 从 baseSn + 1 开始，扫描后续 32 个槽位，生成 uint32 位图
     */
    uint32_t GenerateNackBitmap(uint16_t baseSn) const;

private:
    /**
     * @brief 计算环形索引
     */
    uint16_t GetIndex(uint16_t sn) const;

    // 物理存储：环形数组
    std::vector<Ptr<Packet>> m_buffer; 
    
    // 状态存储：本地位图 (True=已收到, False=空/待接收)
    std::vector<bool> m_isReceived;
    
    uint16_t m_size; // 缓冲区容量
};

} // namespace ns3
