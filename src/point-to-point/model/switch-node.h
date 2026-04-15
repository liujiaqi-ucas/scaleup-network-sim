#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <ns3/node.h>

#include <unordered_map>
#include <unordered_set>

#include "qbb-net-device.h"
#include "switch-mmu.h"

namespace ns3 {

    
// 转发请求的返回状态
enum ForwardStatus {
    FORWARD_SUCCESS = 0,
    BLOCKED_BY_LOCK = 1, // 失败：目标端口正被其他包霸占 (原子性保护)
    BLOCKED_BY_MMU = 2   // 失败：目标端口空闲，但被动态阈值卡住 (降存保护)
};
class Packet;
// 【新增】定义连接表项结构
struct SwitchConnection {
    uint32_t outDev;  // 出端口索引
    uint32_t qIndex;  // 队列/优先级索引 (Body Flit 也要沿用 Head 的优先级)
    bool isValid;     // 是否有效
};


class SwitchNode : public Node {
    static const unsigned qCnt = 8;    // Number of queues/priorities used
    static const unsigned pCnt = 128;  // port 0 is not used so + 1	// Number of ports used
    uint32_t m_ecmpSeed;
    std::unordered_map<uint32_t, std::vector<int> >
        m_rtTable;  // map from ip address (u32) to possible ECMP port (index of dev)

    // monitor uplinks
    uint64_t m_txBytes[pCnt];  // counter of tx bytes, for HPCC
    
   protected:
    bool m_ecnEnabled;
    uint32_t m_ccMode;
    uint32_t m_ackHighPrio;  // set high priority for ACK/NACK

   private:
    uint32_t LookupRoutingTable(Ptr<Packet> flit); // 【新增】包装器
    int GetOutDev(Ptr<Packet>, CustomHeader &ch);
    
    static uint32_t EcmpHash(const uint8_t *key, size_t len, uint32_t seed);
    
    /* Sending packet to Egress port */
    void DoSwitchSend(Ptr<Packet> p, uint32_t outDev, uint32_t qIndex);
   


    /*----- Load balancer -----*/
    // Flow ECMP (lb_mode = 0)
    uint32_t DoLbFlowECMP(Ptr<const Packet> p, const CustomHeader &ch,
                          const std::vector<int> &nexthops);
    // DRILL (lb_mode = 2)
    uint32_t DoLbDrill(Ptr<const Packet> p, const CustomHeader &ch,
                       const std::vector<int> &nexthops);     // choose egress port
    uint32_t m_drill_candidate;                               // always 2 (power of two)
    std::map<uint32_t, uint32_t> m_previousBestInterfaceMap;  // <dip, previousBestInterface>
    uint32_t CalculateInterfaceLoad(uint32_t interface);      // Get the load of a interface
    // Conga (lb_mode = 3)
    uint32_t DoLbConga(Ptr<Packet> p, CustomHeader &ch, const std::vector<int> &nexthops);
    // Conga (lb_mode = 6)
    uint32_t DoLbLetflow(Ptr<Packet> p, CustomHeader &ch, const std::vector<int> &nexthops);
    // ConWeave (lb_mode = 9)
    uint32_t DoLbConWeave(Ptr<const Packet> p, const CustomHeader &ch,
                           const std::vector<int> &nexthops);  // dummy
    
   public:
   //void releasecredit(uint32_t outdev); // 已废弃
    // Ptr<BroadcomNode> m_broadcom;
    Ptr<SwitchMmu> m_mmu;
    bool m_isToR;                                 // true if ToR switch
    std::unordered_set<uint32_t> m_isToR_hostIP;  // host's IP connected to this ToR

    static TypeId GetTypeId(void);
    SwitchNode();
    virtual ~SwitchNode();
    void SetEcmpSeed(uint32_t seed);
    void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
    void ClearTable();
    bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
    void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);
    uint64_t GetTxBytesOutDev(uint32_t outdev);
    //~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    // 互斥锁数组：记录每个 TX 端口当前被哪个 RX 端口“霸占”？
    // 大小为 m_numPorts，值为 -1 表示空闲，值为 rxPortId 表示被锁
    std::vector<int> m_txPortLocks;
    // 【新增】RX 端口活跃路由表：记录每个 RX 端口当前正在处理的包要去哪个 TX 端口
    // 大小为 m_numPorts，值为 -1 表示当前 RX 端口没有在处理任何包
    std::vector<int> m_rxActiveRoutes;
    // 【公平唤醒机制】
    // Lock 等待: per-TX-port FIFO 队列 (解锁时只唤醒等这个端口的人，先来先服务)
    std::deque<int> m_lockWaiters[pCnt];
    std::set<int> m_inLockQueue;   // 快速查重：某个 RX 端口是否已在某个 lock 队列中
    // MMU 等待: 全局 FIFO 队列 (释放空间时逐个唤醒，先来先服务)
    std::deque<int> m_mmuWaiters;
    std::set<int> m_inMmuQueue;    // 快速查重：某个 RX 端口是否已在 MMU 队列中
    // 双重唤醒机制的导火索
    void NotifyLockReleased(int txPortId); // 导火索 1：通道解锁唤醒
    void NotifySpaceAvailable();           // 导火索 2：内存释放唤醒
    void CheckEgressPfcResume();           // 检查所有 egress port 的 XON 条件
    ForwardStatus RequestForward(int rxPortId, Ptr<Packet> flit);
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */









// namespace ns3 {

// // 转发请求的返回状态
// enum ForwardStatus {
//     FORWARD_SUCCESS = 0,
//     BLOCKED_BY_LOCK = 1, // 失败：目标端口正被其他包霸占 (原子性保护)
//     BLOCKED_BY_MMU = 2   // 失败：目标端口空闲，但被动态阈值卡住 (降存保护)
// };

// class Packet;

// class SwitchNode : public Node {
// public:
//     static const unsigned qCnt = 8;    // Number of queues/priorities used
//     static const unsigned pCnt = 128;  // Number of ports used

//     Ptr<SwitchMmu> m_mmu;
//     bool m_isToR;                                 // true if ToR switch
//     std::unordered_set<uint32_t> m_isToR_hostIP;  // host's IP connected to this ToR

//     static TypeId GetTypeId(void);
//     SwitchNode();
    
//     void SetEcmpSeed(uint32_t seed);
//     void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
//     void ClearTable();
//     uint64_t GetTxBytesOutDev(uint32_t outdev);

//     // =========================================================
//     // 【新架构核心接口】
//     // =========================================================
//     // 网卡 RX 端尝试把微片送进交换机时调用
//     ForwardStatus RequestForward(int rxPortId, Ptr<Packet> flit);

//     // 唤醒导火索
//     void NotifyLockReleased(int txPortId); 
//     void NotifySpaceAvailable();           

// protected:
//     bool m_ecnEnabled;
//     uint32_t m_ccMode;
//     uint32_t m_ackHighPrio;  

// private:
//     uint32_t m_ecmpSeed;
//     uint32_t m_drill_candidate; 
//     std::unordered_map<uint32_t, std::vector<int>> m_rtTable; 
//     std::map<uint32_t, uint32_t> m_previousBestInterfaceMap; 
//     uint64_t m_txBytes[pCnt]; 

//     // =========================================================
//     // 【新架构核心状态机变量】
//     // =========================================================
//     // 互斥锁数组：记录每个 TX 端口当前被哪个 RX 端口“霸占” (-1为空闲)
//     std::vector<int> m_txPortLocks;
    
//     // RX 端口活跃路由备忘录：记录 Head 微片查好的路 (-1为无)
//     std::vector<int> m_rxActiveRoutes;
    
//     // 阻塞名单：因为没抢到锁 / 没抢到内存而睡觉的 RX 端口
//     std::set<int> m_blockedByLock; 
//     std::set<int> m_blockedByMMU;  

//     // =========================================================
//     // 路由查找函数 (保留你的原始实现)
//     // =========================================================
//     int GetOutDev(Ptr<Packet> p, CustomHeader &ch);
//     uint32_t LookupRoutingTable(Ptr<Packet> flit); // 【新增】包装器
//     uint32_t DoLbFlowECMP(Ptr<const Packet> p, const CustomHeader &ch, const std::vector<int> &nexthops);
//     uint32_t DoLbDrill(Ptr<const Packet> p, const CustomHeader &ch, const std::vector<int> &nexthops);     
//     uint32_t CalculateInterfaceLoad(uint32_t interface);      
//     uint32_t DoLbConga(Ptr<Packet> p, CustomHeader &ch, const std::vector<int> &nexthops);
//     uint32_t DoLbLetflow(Ptr<Packet> p, CustomHeader &ch, const std::vector<int> &nexthops);
//     uint32_t DoLbConWeave(Ptr<const Packet> p, const CustomHeader &ch, const std::vector<int> &nexthops);  
//     static uint32_t EcmpHash(const uint8_t *key, size_t len, uint32_t seed);
// };

// } /* namespace ns3 */
