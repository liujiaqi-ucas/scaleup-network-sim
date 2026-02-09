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

SwitchNode::SwitchNode() {
    // 1. 先创建对象！
    m_mmu = CreateObject<SwitchMmu>();
    
    // 2. 只有创建了之后，才能调用它的方法
    m_mmu->SetNode(this);
    m_ecmpSeed = m_id;
    m_isToR = false;
    m_node_type = 1;
    m_isToR = false;
    m_drill_candidate = 2;
    
    // Conga's Callback for switch functions
    //m_mmu->m_congaRouting.SetSwitchSendCallback(MakeCallback(&SwitchNode::DoSwitchSend, this));
    //m_mmu->m_congaRouting.SetSwitchSendToDevCallback(
        //MakeCallback(&SwitchNode::SendToDevContinue, this));
    // ConWeave's Callback for switch functions
    //m_mmu->m_conweaveRouting.SetSwitchSendCallback(MakeCallback(&SwitchNode::DoSwitchSend, this));
    //m_mmu->m_conweaveRouting.SetSwitchSendToDevCallback(
        //MakeCallback(&SwitchNode::SendToDevContinue, this));
// 你的逻辑依赖于 -1 代表空闲，如果不初始化，里面是随机垃圾值，一开始就会导致锁死
    for (uint32_t i = 0; i < pCnt; i++) {
        m_portOccupancy[i] = -1; 
    }
    for (uint32_t i = 0; i < pCnt; i++) {
        m_txBytes[i] = 0;
    }
    // 初始化账本
    for(int i=0; i<pCnt; i++)
        for(int j=0; j<qCnt; j++)
            m_cumulativeFreedBytes[i][j] = 0;
    for (uint32_t i = 0; i < pCnt; i++) {
        m_connectionTable[i].outDev = 0;     // 设为0或安全值
        m_connectionTable[i].qIndex = 0;
        m_connectionTable[i].isValid = false; // 必须标记为无效！
    }
}


// 这是一个纯查询函数，不改变任何状态
int32_t SwitchNode::GetPacketDest(Ptr<Packet> p) {
    FlitHeader fh;
    p->PeekHeader(fh);
    
    // 如果是 HEAD，查路由表
    if (fh.GetType() == 0 || fh.GetType() == 3) {
        p->RemoveHeader(fh); // 取出 FlitHeader 以便查路由
       CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
        p->PeekHeader(ch);
        p->AddHeader(fh); // 放回 FlitHeader，保持包不变
        return GetOutDev(p, ch); // 复用你的路由查找逻辑
    }
    
    // 如果是 BODY，理论上不需要唤醒（因为它已经占锁了），
    // 但为了代码健壮性，这里应该查不到 ConnectionTable（因为没传 inDev），
    // 所以这个函数主要服务于 HEAD 包的仲裁。
    return -1;
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
    Ptr<QbbNetDevice> device = DynamicCast<QbbNetDevice>(m_devices[interface]);
    NS_ASSERT_MSG(!!device && !!device->GetQueue(),
                  "Error of getting a egress queue for calculating interface load");
    return device->GetQueue()->GetNBytesTotal();  // also used in HPCC
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

/********************************************
 *              MAIN LOGICS                 *
 *******************************************/


bool SwitchNode::AttemptForward(Ptr<Packet> p, uint32_t inDev) {
    
    
    // 1. 解析包类型
    FlitHeader fh;
    p->PeekHeader(fh);
    uint32_t type = fh.GetType();
    uint32_t outDev = -1;
    // 【调试日志】
    if (type == 0 || type == 3) {
        std::cout << "[DEBUG] Head/Single Pkt at Node " << GetId() 
                  << " inDev " << inDev << " Type=" << type << std::endl;
    }
    // 2. 确定出端口
    if (type == 0 /*HEAD*/ || type == 3 /*SINGLE*/) {
        SwitchDestTag destTag;
        
        // 【优化核心】：先看有没有缓存
        if (p->PeekPacketTag(destTag)) {
            // A. 命中缓存！直接拿结果，无需脱头
            outDev = destTag.GetDest();
        } 
        else {
            // B. 第一次处理（未命中）：执行昂贵的解析
            
            p->RemoveHeader(fh); // 移除 FlitHeader
            
            // 这里的 CustomHeader 构造可能需要根据你的实际情况调整参数
            CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
            
            // 注意：PeekHeader 是不够的，GetOutDev 内部可能需要特定的 Header 结构
            // 如果 GetOutDev 依赖 ch，确保这里正确提取了 ch
            p->PeekHeader(ch); 
            
            p->AddHeader(fh); // 装回去
            
            // 查路由表
            outDev = GetOutDev(p, ch);
            
            // 【关键】：把结果存入 Tag，下次就不用算了
            destTag.SetDest(outDev);
            p->AddPacketTag(destTag);
        }
        

    } else {
        // 查连接表 (Wormhole 机制)
        // 假设你之前存了 connectionTable
        outDev = m_connectionTable[inDev].outDev;
    }
    
    // ---------------------------------------------------------
    // 检查点 1: 端口占用检查 (Wormhole 锁)
    // ---------------------------------------------------------
    int32_t owner = m_portOccupancy[outDev];
    bool isPortFree = (owner == -1);
    bool isOwner = (owner == (int32_t)inDev);

    // 如果我是 HEAD，且端口非空闲 -> 阻塞
    if (type == 0 /*HEAD*/ || type == 3 /*SINGLE*/) {
        if (!isPortFree) {
            std::cout << "Switch " << GetId() << ": Port " << outDev << " locked by " << owner << ". InDev " << inDev << " blocked." << std::endl;
            
            // 【关键修改】注册到“等待解锁”队列 (Wait for Lock)
            
            m_mmu->RegisterWaitPort(outDev, inDev); 
            return false; 
        }
        //std::cout<<"我是head/single,indev是"<<inDev<<"outdev是 "<<outDev
    }
    // 如果我是 BODY/TAIL，但我不是 Owner -> 严重错误 (逻辑不一致)
    else if (!isOwner) {
        // 这通常不应该发生，除非路由表变了或者状态乱了
        std::cout << "CRITICAL ERROR: Body packet from " << inDev << " arrived but port " << outDev << " owned by " << owner << std::endl;
        exit(1); 
    }

    // ---------------------------------------------------------
    // 检查点 2: 空间不足检查 (Credit Check)
    // ---------------------------------------------------------
    // 注意：即使拿到锁了，如果没有空间，也发不出去！
    if (!m_mmu->CheckEgressAdmission(outDev)) {
        std::cout << "Switch " << GetId() << ": Port " << outDev << " buffer full. InDev " << inDev << " blocked." << std::endl;
        
        // 【保持原样】注册到“等待空间”队列 (Wait for Space)
        m_mmu->RegisterWaitSpace(outDev, inDev);
        return false;
    }
    // ---------------------------------------------------------
    // 通过所有检查 -> 发送
    // ---------------------------------------------------------
    
    // 1. 如果是 HEAD，抢锁
    if (type == 0 /*HEAD*/ && isPortFree) {
        m_portOccupancy[outDev] = inDev;
        m_connectionTable[inDev].outDev = outDev;
        m_connectionTable[inDev].isValid = true;
    }

    // 2. 扣除 Credit，更新账本 (保持你的代码)
    SwitchDestTag destTag;
    p->RemovePacketTag(destTag);
    m_mmu->UpdateEgressAdmission(outDev);

    //m_mmu->RemoveFromIngressAdmission(inDev, 3, p->GetSize());//这个好像也不用改，就是一个记账的工作嘛

    // 3. 释放上游 Credit (保持你的代码)
    Ptr<NetDevice> baseDev = m_devices[inDev];
    Ptr<QbbNetDevice> qbbDev = DynamicCast<QbbNetDevice>(baseDev);
    if (qbbDev) {
        qbbDev->ReleaseRxCredit(1);//release这个函数估计也要改一下
    }
     
    // 4. 物理发送
    CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    m_devices[outDev]->SwitchSend(3, p, ch);
    std::cout<<"switch   send!"<<std::endl;
    m_txBytes[outDev] += p->GetSize();

    // 5. 如果是 TAIL，解锁并唤醒等待锁的端口
    if (type == 2 /*TAIL*/ || type == 3 /*SINGLE*/) {
        m_portOccupancy[outDev] = -1; // 解锁
        m_connectionTable[inDev].isValid = false;

        // 【关键修改】唤醒那些因为“端口被锁”而阻塞的入端口
        m_mmu->NotifyOutputPortFree(outDev); 
    }

    return true;
}
void SwitchNode::releasecredit(uint32_t outDev){
    //
   m_mmu->ReleaseEgressAdmission(outDev);
}

// This function can only be called in switch mode
bool SwitchNode::SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet,
                                         CustomHeader &ch) {
    
    uint32_t inDev = device->GetIfIndex();
    std::cout<<"我进到switch  "<<GetId()<<"了"<<",入端口是device "<<inDev<<",要执行mmu的ArbitrateAndSend函数了"<<std::endl;
    m_mmu->ArbitrateAndSend(inDev);//这里直接调用转发函数就行了，尝试一下进行转发
    return true;
}


bool SwitchNode::cantransmit(int inDev){
    Ptr<NetDevice> baseDev = m_devices[inDev];
    Ptr<QbbNetDevice> qbbDev = DynamicCast<QbbNetDevice>(baseDev);
    return qbbDev->cantransmit();
}
int SwitchNode::GetOutDev(Ptr<Packet> p, CustomHeader &ch) {
    // look up entries
    auto entry = m_rtTable.find(ch.dip);

    // no matching entry
    if (entry == m_rtTable.end()) {
        std::cout << "[ERROR] Sw(" << m_id << ")," << PARSE_FIVE_TUPLE(ch)
                  << "No matching entry, so drop this packet at SwitchNode (l3Prot:" << ch.l3Prot
                  << ")" << std::endl;
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
            std::cout << "Unknown lb_mode(" << Settings::lb_mode << ")" << std::endl;
            assert(false);
    }
}



void SwitchNode::SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p) {//
    

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
