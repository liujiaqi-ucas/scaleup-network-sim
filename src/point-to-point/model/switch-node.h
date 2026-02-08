#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <ns3/node.h>

#include <unordered_map>
#include <unordered_set>

#include "qbb-net-device.h"
#include "switch-mmu.h"

namespace ns3 {

    // 定义一个简单的 Tag 用来缓存出端口
class SwitchDestTag : public Tag {
public:
    static TypeId GetTypeId(void) {
        static TypeId tid = TypeId("ns3::SwitchDestTag")
            .SetParent<Tag>()
            .AddConstructor<SwitchDestTag>();
        return tid;
    }
    virtual TypeId GetInstanceTypeId(void) const { return GetTypeId(); }
    virtual uint32_t GetSerializedSize(void) const { return sizeof(uint32_t); }
    
    // 序列化：把 outDev 写入流
    virtual void Serialize(TagBuffer i) const { i.WriteU32(m_outDev); }
    // 反序列化：从流读取 outDev
    virtual void Deserialize(TagBuffer i) { m_outDev = i.ReadU32(); }
    virtual void Print(std::ostream &os) const { os << "Dest=" << m_outDev; }

    // 存取接口
    void SetDest(uint32_t port) { m_outDev = port; }
    uint32_t GetDest() const { return m_outDev; }

private:
    uint32_t m_outDev;
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
    // 【新增】连接表
    // 维度: [入端口数量][VC数量] -> 映射到 出端口信息
    SwitchConnection m_connectionTable[pCnt];
    
   protected:
    bool m_ecnEnabled;
    uint32_t m_ccMode;
    uint32_t m_ackHighPrio;  // set high priority for ACK/NACK

   private:
    int GetOutDev(Ptr<Packet>, CustomHeader &ch);
    void SendToDev(Ptr<Packet> p, CustomHeader &ch);
    void SendToDevContinue(Ptr<Packet> p, CustomHeader &ch);
    static uint32_t EcmpHash(const uint8_t *key, size_t len, uint32_t seed);
    
    // 【新增】 绝对值流控账本 (累计释放量)
    // 维度: [入端口][队列]
    uint64_t m_cumulativeFreedBytes[pCnt][qCnt];
    /* Sending packet to Egress port */
    void DoSwitchSend(Ptr<Packet> p, uint32_t outDev, uint32_t qIndex);
   
   
    // 【出端口占用锁】
    // m_portOccupancy[outDev] = inDev
    // -1 表示空闲；否则表示被入端口 inDev 独占
    int32_t m_portOccupancy[pCnt];


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
   void releasecredit(uint32_t outdev);
    // Ptr<BroadcomNode> m_broadcom;
    Ptr<SwitchMmu> m_mmu;
    bool m_isToR;                                 // true if ToR switch
    std::unordered_set<uint32_t> m_isToR_hostIP;  // host's IP connected to this ToR

    static TypeId GetTypeId(void);
    SwitchNode();
    void SetEcmpSeed(uint32_t seed);
    void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
    void ClearTable();
    bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
    void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);
    uint64_t GetTxBytesOutDev(uint32_t outdev);
    // 【仲裁接口】尝试转发
    // 返回 true 表示成功拿到锁并发送；false 表示忙
    bool AttemptForward(Ptr<Packet> p, uint32_t inDev);

    // 辅助函数：只查路由，不发送 (用于 MMU 唤醒时偷看目的地)
    int32_t GetPacketDest(Ptr<Packet> p);
    bool cantransmit(int inDev);//判断指定端口的包目前能否转发，丢包条件下的
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
