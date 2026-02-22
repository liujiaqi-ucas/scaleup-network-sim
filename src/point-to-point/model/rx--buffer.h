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
     * @brief 生成 128bit NACK 位图
     * 从 baseSn + 1 开始，扫描后续 128 个槽位
     * bit=1 表示已收到，bit=0 表示缺失
     * @param baseSn    基准序列号 (FirstMissing)
     * @param bitmapHigh 输出：高 64 位 (对应 baseSn+65 ~ baseSn+128)
     * @param bitmapLow  输出：低 64 位 (对应 baseSn+1  ~ baseSn+64)
     */
    void GenerateNackBitmap(uint16_t baseSn,
                            uint64_t &bitmapHigh,
                            uint64_t &bitmapLow) const;
     /**
     * @brief 调试用：打印从 firstExpected 开始到最后一个有数据的位置的详细状态
     * @param firstExpected 当前期望收到的第一个 SN（即 rxNext / expected_seq）
     * @param windowSize    最多向后扫描多少个槽位（防止无限扫描）
     */
    void PrintDebugState(uint16_t firstExpected, uint16_t windowSize = 64) const;

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
