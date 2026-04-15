#include "switch-node.h"

#include "assert.h"
#include "ns3/boolean.h"
#include "ns3/conweave-routing.h"
#include "ns3/double.h"
#include "ns3/flow-id-tag.h"
#include "ns3/int-header.h"
#include "ns3/ipv4-header.h"
#include "ns3/ipv4.h"
#include "ns3/letflow-routing.h"
#include "ns3/packet.h"
#include "ns3/pause-header.h"
#include "ns3/settings.h"
#include "ns3/uinteger.h"
#include "ppp-header.h"
#include "qbb-net-device.h"

namespace ns3 {

TypeId SwitchNode::GetTypeId(void) {
    static TypeId tid =
        TypeId("ns3::SwitchNode")
            .SetParent<Node>()
            .AddConstructor<SwitchNode>()
            .AddAttribute("EcnEnabled", "Enable ECN marking.", BooleanValue(false),
                          MakeBooleanAccessor(&SwitchNode::m_ecnEnabled), MakeBooleanChecker())
            .AddAttribute("CcMode", "CC mode.", UintegerValue(0),
                          MakeUintegerAccessor(&SwitchNode::m_ccMode),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("AckHighPrio", "Set high priority for ACK/NACK or not", UintegerValue(0),
                          MakeUintegerAccessor(&SwitchNode::m_ackHighPrio),
                          MakeUintegerChecker<uint32_t>());
    return tid;
}

SwitchNode::~SwitchNode() {}

SwitchNode::SwitchNode() {
    m_node_type = 1;  // 标记为交换机节点
    m_mmu = CreateObject<SwitchMmu>();
    m_mmu->SetNode(this);
    m_ecmpSeed = m_id;
    m_isToR = false;
    m_drill_candidate = 2;
    
    for (uint32_t i = 0; i < pCnt; i++) {
        m_txBytes[i] = 0;
    }

    // 【极其重要的初始化】：防止一开始锁死和越界
    m_txPortLocks.resize(pCnt, -1);
    m_rxActiveRoutes.resize(pCnt, -1);
    
}
// =========================================================
// 【新增桥接函数】：剥离 Flit 头部去查 IP 路由表
// =========================================================
uint32_t SwitchNode::LookupRoutingTable(Ptr<Packet> flit) {
    FlitHeader fh;
    flit->RemoveHeader(fh); 
    
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    flit->PeekHeader(ch); // 获取真实的五元组
    
    flit->AddHeader(fh);  // 查完后必须原封不动地装回去！
    
    return GetOutDev(flit, ch); // 调用你原来的路由分发逻辑
}
// =========================================================
// 核心仲裁引擎：虫洞路由状态机
// =========================================================
ForwardStatus SwitchNode::RequestForward(int rxPortId, Ptr<Packet> flit) {
    int txPortId = -1;
    FlitHeader fh;
    flit->PeekHeader(fh);
    uint32_t type = fh.GetType(); // 0:HEAD, 1:BODY, 2:TAIL, 3:SINGLE

    // -----------------------------------------------------
    // 导航阶段
    // -----------------------------------------------------
    if (type == 0 || type == 3) {
        txPortId = LookupRoutingTable(flit);
        m_rxActiveRoutes[rxPortId] = txPortId; // 记录备忘录
    }
    else if (type == 1 || type == 2) {
        txPortId = m_rxActiveRoutes[rxPortId];
        if (txPortId == -1) {
            NS_LOG_ERROR("Ghost Flit! Body/Tail arrived without a preceding Head flit at RX " << rxPortId);
            return BLOCKED_BY_LOCK;
        }
    }

    // -----------------------------------------------------
    // 阶段一：查锁
    // -----------------------------------------------------
    int currentOwner = m_txPortLocks[txPortId];
    if (currentOwner != -1 && currentOwner != rxPortId) {
        if (m_inLockQueue.find(rxPortId) == m_inLockQueue.end()) {
            m_lockWaiters[txPortId].push_back(rxPortId);
            m_inLockQueue.insert(rxPortId);
        }
        return BLOCKED_BY_LOCK;
    }

    // -----------------------------------------------------
    // 阶段二：查 MMU
    // -----------------------------------------------------
    int index = m_mmu->AllocateSpace(txPortId);
    if (index == -1) {
        if (m_inMmuQueue.find(rxPortId) == m_inMmuQueue.end()) {
            m_mmuWaiters.push_back(rxPortId);
            m_inMmuQueue.insert(rxPortId);
        }
        // PFC: MMU 满了，让被阻塞的 RX 端口向上游发 PAUSE
        Ptr<QbbNetDevice> rxDev = DynamicCast<QbbNetDevice>(GetDevice(rxPortId));
        if (rxDev && rxDev->IsQbbEnabled() && !rxDev->m_pfcPauseSent) {
            rxDev->m_pfcPauseSent = true;
            rxDev->SendPfc(0, 0);
        }
        return BLOCKED_BY_MMU;
    }

    // -----------------------------------------------------
    // 阶段三：存放与状态转移
    // -----------------------------------------------------
    m_mmu->StorePacket(index, flit); // 零拷贝存入金库
    Ptr<QbbNetDevice> txDevice = DynamicCast<QbbNetDevice>(GetDevice(txPortId));

    if (txDevice) {
        txDevice->EnqueueTxIndex(index);
        txDevice->DequeueAndTransmit();
    }
    // 通知 RX 设备发送待发的 credit（RX 端口在转发成功后需要告知上游可继续发送）
    Ptr<QbbNetDevice> rxDevice = DynamicCast<QbbNetDevice>(GetDevice(rxPortId));
    if (rxDevice) {
        rxDevice->TriggerCreditSendIfNeeded();
    }
    if (type == 0 || type == 3) {
        m_txPortLocks[txPortId] = rxPortId; // 火车头上锁
    }

    if (type == 2 || type == 3) {
        m_txPortLocks[txPortId] = -1;       // 火车尾解锁
        m_rxActiveRoutes[rxPortId] = -1;    // 擦除导航记录

        NotifyLockReleased(txPortId);       // 叫醒等这个 TX 端口的下一个人
    }

    // 冲关成功，从跟踪集合中清除（可能已被 Notify 弹出，erase 是幂等的）
    m_inLockQueue.erase(rxPortId);
    m_inMmuQueue.erase(rxPortId);

    return FORWARD_SUCCESS;
}

// =========================================================
// 【公平唤醒导火索 1】：Lock 释放 —— per-TX-port FIFO
// =========================================================
// 只唤醒等这个特定 TX 端口的 RX 端口，按 FIFO 顺序逐个尝试。
// 一旦有人成功抢到锁（txPortLocks 被占），立刻停止。
// 如果队头因为 MMU 满而失败，继续尝试下一个（锁还是空闲的）。
void SwitchNode::NotifyLockReleased(int txPortId) {
    // 收集需要唤醒的端口，但不立即调用 TryForwardingRxBuffer
    // 用 ScheduleNow 延迟到下一个事件循环迭代，避免同步级联导致指数爆炸
    while (!m_lockWaiters[txPortId].empty()) {
        int rxPortId = m_lockWaiters[txPortId].front();
        m_lockWaiters[txPortId].pop_front();
        m_inLockQueue.erase(rxPortId);

        Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(GetDevice(rxPortId));
        if (dev) {
            Simulator::ScheduleNow(&QbbNetDevice::TryForwardingRxBuffer, dev);
        }
        // 不再同步检查锁状态——延迟执行后锁的状态会自然处理
        break;  // 只唤醒一个，后续的等下次释放时再唤醒
    }
}

// =========================================================
// 【公平唤醒导火索 2】：MMU 释放 —— 全局 FIFO
// =========================================================
// 逐个唤醒，直到空闲空间耗尽。
// 这样每次释放 1 个 flit 只唤醒 1 个等待者，消除雪崩效应。
void SwitchNode::NotifySpaceAvailable() {
    // 与 NotifyLockReleased 同理：延迟唤醒，避免同步级联
    if (!m_mmuWaiters.empty()) {
        int rxPortId = m_mmuWaiters.front();
        m_mmuWaiters.pop_front();
        m_inMmuQueue.erase(rxPortId);

        Ptr<QbbNetDevice> dev = DynamicCast<QbbNetDevice>(GetDevice(rxPortId));
        if (dev) {
            // PFC: MMU 有空间了，让之前被暂停的端口发 RESUME 上游
            if (dev->IsQbbEnabled() && dev->m_pfcPauseSent) {
                dev->m_pfcPauseSent = false;
                dev->SendPfc(0, 1);
            }
            Simulator::ScheduleNow(&QbbNetDevice::TryForwardingRxBuffer, dev);
        }
    }
}


/**
 * @brief Load Balancing
 */
uint32_t SwitchNode::DoLbFlowECMP(Ptr<const Packet> p, const CustomHeader &ch,
                                  const std::vector<int> &nexthops) {
    // pick one next hop based on hash
    union {
        uint8_t u8[4 + 4 + 2 + 2];
        uint32_t u32[3];
    } buf;
    buf.u32[0] = ch.sip;
    buf.u32[1] = ch.dip;
    if (ch.l3Prot == 0x6)
        buf.u32[2] = ch.tcp.sport | ((uint32_t)ch.tcp.dport << 16);
    else if (ch.l3Prot == 0x11)  // XXX RDMA traffic on UDP
        buf.u32[2] = ch.udp.sport | ((uint32_t)ch.udp.dport << 16);
    else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD)  // ACK or NACK
        buf.u32[2] = ch.ack.sport | ((uint32_t)ch.ack.dport << 16);
    else {
        std::cout << "[ERROR] Sw(" << m_id << ")," << PARSE_FIVE_TUPLE(ch)
                  << "Cannot support other protoocls than TCP/UDP (l3Prot:" << ch.l3Prot << ")"
                  << std::endl;
        assert(false && "Cannot support other protoocls than TCP/UDP");
    }

    uint32_t hashVal = EcmpHash(buf.u8, 12, m_ecmpSeed);
    uint32_t idx = hashVal % nexthops.size();
    return nexthops[idx];
}

/*-----------------CONGA-----------------*/
uint32_t SwitchNode::DoLbConga(Ptr<Packet> p, CustomHeader &ch, const std::vector<int> &nexthops) {
    return DoLbFlowECMP(p, ch, nexthops);  // flow ECMP (dummy)
}

/*-----------------Letflow-----------------*/
uint32_t SwitchNode::DoLbLetflow(Ptr<Packet> p, CustomHeader &ch,
                                 const std::vector<int> &nexthops) {
    if (m_isToR && nexthops.size() == 1) {
        if (m_isToR_hostIP.find(ch.sip) != m_isToR_hostIP.end() &&
            m_isToR_hostIP.find(ch.dip) != m_isToR_hostIP.end()) {
            return nexthops[0];  // intra-pod traffic
        }
    }

    /* ONLY called for inter-Pod traffic */
    uint32_t outPort = m_mmu->m_letflowRouting.RouteInput(p, ch);
    if (outPort == LETFLOW_NULL) {
        assert(nexthops.size() == 1);  // Receiver's TOR has only one interface to receiver-server
        outPort = nexthops[0];         // has only one option
    }
    assert(std::find(nexthops.begin(), nexthops.end(), outPort) !=
           nexthops.end());  // Result of Letflow cannot be found in nexthops
    return outPort;
}

/*-----------------DRILL-----------------*/
uint32_t SwitchNode::CalculateInterfaceLoad(uint32_t interface) {
    // BEgressQueue 已移除，返回 0 (DRILL/HPCC 负载均衡未使用)
    return 0;
}

uint32_t SwitchNode::DoLbDrill(Ptr<const Packet> p, const CustomHeader &ch,
                               const std::vector<int> &nexthops) {
    // find the Egress (output) link with the smallest local Egress Queue length
    uint32_t leastLoadInterface = 0;
    uint32_t leastLoad = std::numeric_limits<uint32_t>::max();
    auto rand_nexthops = nexthops;
    std::random_shuffle(rand_nexthops.begin(), rand_nexthops.end());

    std::map<uint32_t, uint32_t>::iterator itr = m_previousBestInterfaceMap.find(ch.dip);
    if (itr != m_previousBestInterfaceMap.end()) {
        leastLoadInterface = itr->second;
        leastLoad = CalculateInterfaceLoad(itr->second);
    }

    uint32_t sampleNum =
        m_drill_candidate < rand_nexthops.size() ? m_drill_candidate : rand_nexthops.size();
    for (uint32_t samplePort = 0; samplePort < sampleNum; samplePort++) {
        uint32_t sampleLoad = CalculateInterfaceLoad(rand_nexthops[samplePort]);
        if (sampleLoad < leastLoad) {
            leastLoad = sampleLoad;
            leastLoadInterface = rand_nexthops[samplePort];
        }
    }
    m_previousBestInterfaceMap[ch.dip] = leastLoadInterface;
    return leastLoadInterface;
}

/*------------------ConWeave Dummy ----------------*/
uint32_t SwitchNode::DoLbConWeave(Ptr<const Packet> p, const CustomHeader &ch,
                                  const std::vector<int> &nexthops) {
    return DoLbFlowECMP(p, ch, nexthops);  // flow ECMP (dummy)
}
/*----------------------------------*/


// void SwitchNode::releasecredit(uint32_t outDev){
//     //
//    m_mmu->ReleaseEgressAdmission(outDev);
// }

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet,
                                         CustomHeader &ch) {
    // 新架构下，收包由 QbbNetDevice::Receive 处理，这里仅返回 true
    return true;
}



int SwitchNode::GetOutDev(Ptr<Packet> p, CustomHeader &ch) {
    // look up entries
    auto entry = m_rtTable.find(ch.dip);

    // no matching entry
    if (entry == m_rtTable.end()) {
        //std::cout << "[ERROR] Sw(" << m_id << ")," << PARSE_FIVE_TUPLE(ch)
                  //<< "No matching entry, so drop this packet at SwitchNode (l3Prot:" << ch.l3Prot
                  //<< ")" << std::endl;
        assert(false);
    }

    // entry found
    const auto &nexthops = entry->second;
    bool control_pkt =
        (ch.l3Prot == 0xFF || ch.l3Prot == 0xFE || ch.l3Prot == 0xFD || ch.l3Prot == 0xFC);

    if (Settings::lb_mode == 0 || control_pkt) {  // control packet (ACK, NACK, PFC, QCN)
        return DoLbFlowECMP(p, ch, nexthops);     // ECMP routing path decision (4-tuple)
    }

    switch (Settings::lb_mode) {
        case 2:
            return DoLbDrill(p, ch, nexthops);
        case 3:
            return DoLbConga(p, ch, nexthops); /** DUMMY: Do ECMP */
        case 6:
            return DoLbLetflow(p, ch, nexthops);
        case 9:
            return DoLbConWeave(p, ch, nexthops); /** DUMMY: Do ECMP */
        default:
            //std::cout << "Unknown lb_mode(" << Settings::lb_mode << ")" << std::endl;
            assert(false);
    }
}

void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {
    m_txBytes[ifIndex] += p->GetSize();
}

uint32_t SwitchNode::EcmpHash(const uint8_t *key, size_t len, uint32_t seed) {
    uint32_t h = seed;
    if (len > 3) {
        const uint32_t *key_x4 = (const uint32_t *)key;
        size_t i = len >> 2;
        do {
            uint32_t k = *key_x4++;
            k *= 0xcc9e2d51;
            k = (k << 15) | (k >> 17);
            k *= 0x1b873593;
            h ^= k;
            h = (h << 13) | (h >> 19);
            h += (h << 2) + 0xe6546b64;
        } while (--i);
        key = (const uint8_t *)key_x4;
    }
    if (len & 3) {
        size_t i = len & 3;
        uint32_t k = 0;
        key = &key[i - 1];
        do {
            k <<= 8;
            k |= *key--;
        } while (--i);
        k *= 0xcc9e2d51;
        k = (k << 15) | (k >> 17);
        k *= 0x1b873593;
        h ^= k;
    }
    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

void SwitchNode::SetEcmpSeed(uint32_t seed) { m_ecmpSeed = seed; }

void SwitchNode::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx) {
    uint32_t dip = dstAddr.Get();
    m_rtTable[dip].push_back(intf_idx);
}

void SwitchNode::ClearTable() { m_rtTable.clear(); }

uint64_t SwitchNode::GetTxBytesOutDev(uint32_t outdev) {
    assert(outdev < pCnt);
    return m_txBytes[outdev];
}

} /* namespace ns3 */
