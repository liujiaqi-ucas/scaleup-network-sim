#include "rdma-hw.h"

#include <ns3/ipv4-header.h>
#include <ns3/seq-ts-header.h>
#include <ns3/simulator.h>
#include <ns3/udp-header.h>

#include <climits>

#include "cn-header.h"
#include "flow-stat-tag.h"
#include "ns3/boolean.h"
#include "ns3/data-rate.h"
#include "ns3/double.h"
#include "ns3/flow-id-num-tag.h"
#include "ns3/pointer.h"
#include "ns3/ppp-header.h"
#include "ns3/settings.h"
#include "ns3/switch-node.h"
#include "ns3/uinteger.h"
#include "ppp-header.h"
#include "qbb-header.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE("RdmaHw");

std::unordered_map<unsigned, unsigned> acc_timeout_count;
uint64_t RdmaHw::nAllPkts = 0;

TypeId RdmaHw::GetTypeId(void) {
    static TypeId tid =
        TypeId("ns3::RdmaHw")
            .SetParent<Object>()
            .AddAttribute("MinRate", "Minimum rate of a throttled flow",
                          DataRateValue(DataRate("100Mb/s")),
                          MakeDataRateAccessor(&RdmaHw::m_minRate), MakeDataRateChecker())
            .AddAttribute("Mtu", "Mtu.", UintegerValue(1392), MakeUintegerAccessor(&RdmaHw::m_mtu),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("CcMode", "which mode of DCQCN is running", UintegerValue(0),
                          MakeUintegerAccessor(&RdmaHw::m_cc_mode), MakeUintegerChecker<uint32_t>())
            // .AddAttribute("NACKGenerationInterval", "The NACK/CNP Generation interval",
            //               DoubleValue(4.0), MakeDoubleAccessor(&RdmaHw::m_nack_interval),
            //               MakeDoubleChecker<double>())
            .AddAttribute("L2ChunkSize", "Layer 2 chunk size. Disable chunk mode if equals to 0.",
                          UintegerValue(4000), MakeUintegerAccessor(&RdmaHw::m_chunk),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("L2AckInterval", "Layer 2 Ack intervals. Disable ack if equals to 0.",
                          UintegerValue(1), MakeUintegerAccessor(&RdmaHw::m_ack_interval),
                          MakeUintegerChecker<uint32_t>())
            .AddAttribute("L2BackToZero", "Layer 2 go back to zero transmission.",
                          BooleanValue(false), MakeBooleanAccessor(&RdmaHw::m_backto0),
                          MakeBooleanChecker())
            // .AddAttribute("EwmaGain",
            //               "Control gain parameter which determines the level of rate decrease",
            //               DoubleValue(1.0 / 16), MakeDoubleAccessor(&RdmaHw::m_g),
            //               MakeDoubleChecker<double>())
            // .AddAttribute("RateOnFirstCnp", "the fraction of rate on first CNP", DoubleValue(1.0),
            //               MakeDoubleAccessor(&RdmaHw::m_rateOnFirstCNP),
            //               MakeDoubleChecker<double>())
            // .AddAttribute("ClampTargetRate", "Clamp target rate.", BooleanValue(false),
            //               MakeBooleanAccessor(&RdmaHw::m_EcnClampTgtRate), MakeBooleanChecker())
            // .AddAttribute("RPTimer", "The rate increase timer at RP in microseconds",
            //               DoubleValue(300.0), MakeDoubleAccessor(&RdmaHw::m_rpgTimeReset),
            //               MakeDoubleChecker<double>())
            // .AddAttribute("RateDecreaseInterval", "The interval of rate decrease check",
            //               DoubleValue(4.0), MakeDoubleAccessor(&RdmaHw::m_rateDecreaseInterval),
            //               MakeDoubleChecker<double>())
            // .AddAttribute("FastRecoveryTimes", "The rate increase timer at RP", UintegerValue(1),
            //               MakeUintegerAccessor(&RdmaHw::m_rpgThreshold),
            //               MakeUintegerChecker<uint32_t>())
            // .AddAttribute("AlphaResumInterval", "The interval of resuming alpha", DoubleValue(1.0),
            //               MakeDoubleAccessor(&RdmaHw::m_alpha_resume_interval),
            //               MakeDoubleChecker<double>())
            // .AddAttribute("RateAI", "Rate increment unit in AI period",
            //               DataRateValue(DataRate("5Mb/s")), MakeDataRateAccessor(&RdmaHw::m_rai),
            //               MakeDataRateChecker())
            // .AddAttribute("RateHAI", "Rate increment unit in hyperactive AI period",
            //               DataRateValue(DataRate("50Mb/s")), MakeDataRateAccessor(&RdmaHw::m_rhai),
            //               MakeDataRateChecker())
            .AddAttribute("VarWin", "Use variable window size or not", BooleanValue(false),
                          MakeBooleanAccessor(&RdmaHw::m_var_win), MakeBooleanChecker())
            .AddAttribute("FastReact", "Fast React to congestion feedback", BooleanValue(true),
                          MakeBooleanAccessor(&RdmaHw::m_fast_react), MakeBooleanChecker())
            // .AddAttribute("MiThresh", "Threshold of number of consecutive AI before MI",
            //               UintegerValue(5), MakeUintegerAccessor(&RdmaHw::m_miThresh),
            //               MakeUintegerChecker<uint32_t>())
            // .AddAttribute("TargetUtil",
            //               "The Target Utilization of the bottleneck bandwidth, by default 95%",
            //               DoubleValue(0.95), MakeDoubleAccessor(&RdmaHw::m_targetUtil),
            //               MakeDoubleChecker<double>())
            // .AddAttribute(
            //     "UtilHigh",
            //     "The upper bound of Target Utilization of the bottleneck bandwidth, by default 98%",
            //     DoubleValue(0.98), MakeDoubleAccessor(&RdmaHw::m_utilHigh),
            //     MakeDoubleChecker<double>())
            .AddAttribute("RateBound", "Bound packet sending by rate, for test only",
                          BooleanValue(true), MakeBooleanAccessor(&RdmaHw::m_rateBound),
                          MakeBooleanChecker());
            // .AddAttribute("MultiRate", "Maintain multiple rates in HPCC", BooleanValue(true),
            //               MakeBooleanAccessor(&RdmaHw::m_multipleRate), MakeBooleanChecker())
            // .AddAttribute("SampleFeedback", "Whether sample feedback or not", BooleanValue(false),
            //               MakeBooleanAccessor(&RdmaHw::m_sampleFeedback), MakeBooleanChecker())
            // .AddAttribute("TimelyAlpha", "Alpha of TIMELY", DoubleValue(0.875),
            //               MakeDoubleAccessor(&RdmaHw::m_tmly_alpha), MakeDoubleChecker<double>())
            // .AddAttribute("TimelyBeta", "Beta of TIMELY", DoubleValue(0.8),
            //               MakeDoubleAccessor(&RdmaHw::m_tmly_beta), MakeDoubleChecker<double>())
            // .AddAttribute("TimelyTLow", "TLow of TIMELY (ns)", UintegerValue(50000),
            //               MakeUintegerAccessor(&RdmaHw::m_tmly_TLow),
            //               MakeUintegerChecker<uint64_t>())
            // .AddAttribute("TimelyTHigh", "THigh of TIMELY (ns)", UintegerValue(500000),
            //               MakeUintegerAccessor(&RdmaHw::m_tmly_THigh),
            //               MakeUintegerChecker<uint64_t>())
            // .AddAttribute("TimelyMinRtt", "MinRtt of TIMELY (ns)", UintegerValue(20000),
            //               MakeUintegerAccessor(&RdmaHw::m_tmly_minRtt),
            //               MakeUintegerChecker<uint64_t>())
            // .AddAttribute("DctcpRateAI", "DCTCP's Rate increment unit in AI period",
            //               DataRateValue(DataRate("1000Mb/s")),
            //               MakeDataRateAccessor(&RdmaHw::m_dctcp_rai), MakeDataRateChecker())
            // .AddAttribute("IrnEnable", "Enable IRN", BooleanValue(false),
            //               MakeBooleanAccessor(&RdmaHw::m_irn), MakeBooleanChecker())
            // .AddAttribute("IrnRtoLow", "Low RTO for IRN", TimeValue(MicroSeconds(454)),
            //               MakeTimeAccessor(&RdmaHw::m_irn_rtoLow), MakeTimeChecker())
            // .AddAttribute("IrnRtoHigh", "High RTO for IRN", TimeValue(MicroSeconds(1350)),
            //               MakeTimeAccessor(&RdmaHw::m_irn_rtoHigh), MakeTimeChecker())
            // .AddAttribute("IrnBdp", "BDP Limit for IRN in Bytes", UintegerValue(100000),
            //               MakeUintegerAccessor(&RdmaHw::m_irn_bdp), MakeUintegerChecker<uint32_t>())
            // .AddAttribute("L2Timeout", "Sender's timer of waiting for the ack",
            //               TimeValue(MilliSeconds(4)), MakeTimeAccessor(&RdmaHw::m_waitAckTimeout),
            //               MakeTimeChecker());
    return tid;
}

RdmaHw::RdmaHw() {
    //m_currentRxQp = 0;
    
}

void RdmaHw::SetNode(Ptr<Node> node) { m_node = node; }
void RdmaHw::Setup(QpCompleteCallback cb) {
    for (uint32_t i = 0; i < m_nic.size(); i++) {
        Ptr<QbbNetDevice> dev = m_nic[i].dev;
        if (dev == NULL) continue;
        // share data with NIC
        dev->m_rdmaEQ->m_qpGrp = m_nic[i].qpGrp;
        // setup callback
        dev->m_rdmaReceiveCb = MakeCallback(&RdmaHw::Receive, this);
        dev->m_rdmaLinkDownCb = MakeCallback(&RdmaHw::SetLinkDown, this);
        dev->m_rdmaPktSent = MakeCallback(&RdmaHw::PktSent, this);
        // config NIC
        dev->m_rdmaEQ->m_mtu = m_mtu;
        dev->m_rdmaEQ->m_rdmaGetNxtPkt = MakeCallback(&RdmaHw::GetNxtPacket, this);
        // =========================================================
        // 【新增】绑定 DeleteQueuePair
        // =========================================================
        // 意思：当 device 调用 m_txQpFinishCb 时，实际上执行的是 RdmaHw::DeleteQueuePair
        dev->m_rdmaEQ->m_txQpFinishCb = MakeCallback(&RdmaHw::DeleteQueuePair, this);
    }
    // setup qp complete callback
    m_qpCompleteCallback = cb;
}

uint32_t RdmaHw::GetNicIdxOfQp(Ptr<RdmaQueuePair> qp) {
    auto &v = m_rtTable[qp->dip.Get()];
    if (v.size() > 0) {
        return v[qp->GetHash() % v.size()];
    }
    NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
    //std::cout << "We assume at least one NIC is alive" << std::endl;
    exit(1);
}

uint64_t RdmaHw::GetQpKey(uint32_t dip, uint16_t sport, uint16_t dport,
                          uint16_t pg) {  // Sender perspective
    return ((uint64_t)dip << 32) | ((uint64_t)sport << 16) | (uint64_t)dport | (uint64_t)pg;
}
Ptr<RdmaQueuePair> RdmaHw::GetQp(uint64_t key) {
    auto it = m_qpMap.find(key);

    // lookup main memory
    if (it != m_qpMap.end()) {
        return it->second;
    }

    return NULL;
}
void RdmaHw::AddQueuePair(uint64_t size, uint16_t pg, Ipv4Address sip, Ipv4Address dip,
                          uint16_t sport, uint16_t dport, uint32_t win, uint64_t baseRtt,
                          int32_t flow_id) {
    // create qp
    Ptr<RdmaQueuePair> qp = CreateObject<RdmaQueuePair>(pg, sip, dip, sport, dport);
    qp->SetSize(size);
    //qp->SetWin(win);
    //qp->SetBaseRtt(baseRtt);
    //qp->SetVarWin(m_var_win);
    qp->SetFlowId(flow_id);
    //qp->SetTimeout(m_waitAckTimeout);

    // add qp
    uint32_t nic_idx = GetNicIdxOfQp(qp);
    m_nic[nic_idx].qpGrp->AddQp(qp);
    uint64_t key = GetQpKey(dip.Get(), sport, dport, pg);
    m_qpMap[key] = qp;
    // set init variables
    DataRate m_bps = m_nic[nic_idx].dev->GetDataRate();
    qp->m_rate = m_bps;
    qp->m_max_rate = m_bps;
    // Notify Nic
    m_nic[nic_idx].dev->NewQp(qp);
}

void RdmaHw::DeleteQueuePair(Ptr<RdmaQueuePair> qp) {
    // remove qp from the m_qpMap
    uint64_t key = GetQpKey(qp->dip.Get(), qp->sport, qp->dport, qp->m_pg);

    // record to Akashic record
    NS_ASSERT(akashic_Qp.find(key) == akashic_Qp.end());  // should not be already existing
    akashic_Qp.insert(key);

    // delete
    m_qpMap.erase(key);
}

// DATA UDP's src = this key's dst (receiver's dst)
uint64_t RdmaHw::GetRxQpKey(uint32_t dip, uint16_t dport, uint16_t sport,
                            uint16_t pg) {  // Receiver perspective
    return ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | ((uint64_t)sport << 16) |
           (uint64_t)dport;  // srcIP, srcPort
}

// src/dst are already flipped (this is calleld by UDP Data packet)
Ptr<RdmaRxQueuePair> RdmaHw::GetRxQp(uint32_t sip, uint32_t dip, uint16_t sport, uint16_t dport,
                                     uint16_t pg, bool create) {
    uint64_t rxKey = GetRxQpKey(dip, dport, sport, pg);
    auto it = m_rxQpMap.find(rxKey);

    // main memory lookup
    if (it != m_rxQpMap.end()) return it->second;

    if (create) {
        // create new rx qp
        Ptr<RdmaRxQueuePair> q = CreateObject<RdmaRxQueuePair>();
        // init the qp
        q->sip = sip;
        q->dip = dip;
        q->sport = sport;
        q->dport = dport;
        //q->m_ecn_source.qIndex = pg;
        q->m_flow_id = -1;     // unknown
        m_rxQpMap[rxKey] = q;  // store in map
        std::cout<<"Node "<<m_node->GetId()<<" 创建了新的一个 RxQp  "<<std::endl;
        return q;
    }
    return NULL;
}
uint32_t RdmaHw::GetNicIdxOfRxQp(Ptr<RdmaRxQueuePair> q) {
    auto &v = m_rtTable[q->dip];
    if (v.size() > 0) {
        return v[q->GetHash() % v.size()];
    }
    NS_ASSERT_MSG(false, "We assume at least one NIC is alive");
    std::cout << "We assume at least one NIC is alive" << std::endl;
    exit(1);
}
// void RdmaHw::DeleteRxQp(Ptr<RdmaRxQueuePair> q) {
//     if (q) {
//         // 从 q 对象中提取 key 所需的参数
//         // 注意：GetRxQpKey 的参数顺序是 (dip, dport, sport, pg)
//         // 这里的 q->dip/sip 是整数 IP，q->dport/sport 是端口
//         // 这里的 pg 我们可能需要从 q 里取，或者默认 0
//         // 假设 RdmaRxQueuePair 里没有存 pg，通常需要加上，或者传参
        
//         // 仔细看你的 RxQP 定义，通常应该有 m_pg 或者类似字段
//         // 如果没有，你需要用原始的那个函数
        
//         // 假设 q->m_flow_id 对应的 pg (在你的 Step 逻辑里 pg 通常是 3)
//         // 为了稳妥，建议直接调用原始版本，参数从 q 里取：
//         // DeleteRxQp(q->dip, q->dport, q->sport, q->m_pg); <--- 确保 RxQP 有这些成员
        
//         // 修正：GetRxQpKey 需要的参数顺序
//         // key = ((uint64_t)dip << 32) | ((uint64_t)pg << 16) | ((uint64_t)sport << 16) | (uint64_t)dport;
        
//         // 我们直接复用原函数逻辑：
//         DeleteRxQp(q->dip, q->dport, q->sport, 3); // ⚠️ 注意：pg 参数这里如果是 3 (Priority Group)
//     }
// }
// Receiver's perspective?
void RdmaHw::DeleteRxQp(uint32_t dip, uint16_t dport, uint16_t sport, uint16_t pg) {
    uint64_t key = GetRxQpKey(dip, dport, sport, pg);

    // record to Akashic record
    NS_ASSERT(akashic_RxQp.find(key) == akashic_RxQp.end());  // should not be already existing
    akashic_RxQp.insert(key);
    std::cout<<"Node "<<m_node->GetId()<<" 删除了一个  RxQp  "<<std::endl;
    // delete
    m_rxQpMap.erase(key);
}

int RdmaHw::ReceiveUdp(Ptr<Packet> p, CustomHeader &ch,uint32_t dev_idx) {
    
    // 1. 获取 Flit 基础信息
    FlitHeader fh;
    p->PeekHeader(fh); 
    uint8_t type = fh.GetType(); // 0=HEAD, 1=BODY, 2=TAIL, 3=SINGLE
    
    // 获取当前 Flit 的物理 Payload 大小 (扣除 FlitHeader 后的长度)
    // 注意：GetSize() 是包的总长，GetSerializedSize() 是 Flit头 的长度
    uint32_t rawPayloadSize = p->GetSize() - fh.GetSerializedSize();

    // 初始化 QP 指针
    Ptr<RdmaRxQueuePair> rxQp = m_currentRxQpPerDev[dev_idx];
    uint32_t nodeId = m_node->GetId();

    // =========================================================
    // 阶段 A: QP 查找与初始化 (针对 HEAD/SINGLE)
    // =========================================================
    if (type == 0 || type == 3) {
        // 临时移除 FlitHeader 以便读取 CustomHeader (路由/流标识信息)
        p->RemoveHeader(fh); 
        p->PeekHeader(ch);   
        
        rxQp = GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg, true);
        if (rxQp == NULL) {
            // 找不到 QP，可能是已经结束的流
            uint64_t rxKey = GetRxQpKey(ch.sip, ch.udp.sport, ch.udp.dport, ch.udp.pg);
            if (akashic_RxQp.find(rxKey) != akashic_RxQp.end()) {
                return 1; // Drop duplicated packet for finished flow
            } else {
                printf("ERROR: UDP NIC cannot find the flow\n");
                exit(1);
            }
        }
        //std::cout<<" Node "<<nodeId<<" receive packet "<<ch.udp.seq<<"  Expected Packet is  "<<rxQp->expected_seq<<std::endl;
    if (ch.udp.seq != rxQp->expected_seq) { // 校验序号
        //报错，这是不正常的，说明机制有问题
        printf("ERROR: UDP NIC received out-of-order flit seq %u, expected %u\n",
               ch.udp.seq, rxQp->expected_seq);
               exit(1);

    }
    //更新一下流的id
    if (rxQp->m_flow_id < 0) {
        FlowIDNUMTag fit;
        if (p->PeekPacketTag(fit)) {
            rxQp->m_flow_id = fit.GetId();
        }
    }
    std::cout<<"rxQp->expected_seq要加的fh.GetPktTotalBytes()  =  "<<fh.GetPktTotalBytes()<<std::endl;
    rxQp->expected_seq+=fh.GetPktTotalBytes(); // 更新期望的下一个序号
        // 把 FlitHeader 加回去，保持包的完整性 (如果后续还需要处理)
        // 或者因为我们已经拿到了 rawPayloadSize，这里不加回去也行，看后续逻辑
        p->AddHeader(fh); 

        // 更新缓存
        m_currentRxQpPerDev[dev_idx] = rxQp;
    }

    

    // =========================================================
    // 阶段 B: 提取流元数据 (Size & StartTime)
    // =========================================================
    double flowStartTime = 0;
    
    // 尝试从 Tag 中恢复流的总大小 (rxQp->m_size) 和 开始时间
    // 只要 rxQp->m_size 还是 0，就说明我们还没拿到“总任务书”
    if (rxQp->m_size == 0) {
        
        FlowIDNUMTag fint;
        if (p->PeekPacketTag(fint)) {
            rxQp->m_size = fint.GetFlowSize();
            std::cout<<"我是Node "<<m_node->GetId()<<"收到流ID为"<<rxQp->m_flow_id<<"的流的总大小是"<<rxQp->m_size<<std::endl;
        }
    }
    
    // 每次都尝试拿时间戳，因为 FCT 计算依赖它
    FlowStatTag fst;
    if (p->PeekPacketTag(fst)) {
        flowStartTime = fst.getFlowStartTime(); // 使用我们之前加的专用字段
    }


    // =========================================================
    // 阶段 D: 精准数据累加 (Data Accumulation)
    // =========================================================
    uint32_t effectiveDataBytes = 0;

    if (type == 0 || type == 3) { 
        // >>> HEAD 或 SINGLE 包 <<<
        // 结构: [FlitHeader | Protocol Headers (IP/UDP/etc) | Actual Data]
        // 我们只统计 Actual Data
        if (rawPayloadSize >= PROTOCOL_HEADER_SIZE) {
            effectiveDataBytes = rawPayloadSize - PROTOCOL_HEADER_SIZE;
        } else {
            // 异常：包太小，连头都装不下？可能是纯控制包或错误
            effectiveDataBytes = 0; 
        }
    } else {
        // >>> BODY 或 TAIL 包 <<<
        // 结构: [FlitHeader | Actual Data]
        // 全都是数据
        effectiveDataBytes = rawPayloadSize;
    }

     // 累加到 QP 中
    rxQp->received_bytes += effectiveDataBytes;
    uint32_t srcId = Settings::ip_to_node_id(Ipv4Address(rxQp->sip));
    uint32_t dstId = Settings::ip_to_node_id(Ipv4Address(rxQp->dip));
    //std::cout << "  我是Node  " << nodeId << " 累计收到流ID为" << rxQp->m_flow_id
    //<<" 我的流是从 "<<srcId<<" 发来的，发往 "<<dstId
              //<< " Raw: " << rawPayloadSize 
              //<< " Effective: " << effectiveDataBytes 
              //<< " TotalRecv: " << rxQp->received_bytes 
              //<< " / Target: " << rxQp->m_size << std::endl;

    // =========================================================
    // 阶段 E: 流结束判断 (Finish Check)
    // =========================================================
    // 只有当 累计接收的数据量 >= 预设的总大小时，才算真正结束
    // 且必须知道 m_size (防止 m_size 为 0 时的误判)
    if (rxQp->m_size > 0 && rxQp->received_bytes >= rxQp->m_size) {
        
        std::cout << "!!! FLOW FINISH DETECTED FlowID: " << rxQp->m_flow_id << std::endl;

        if (flowStartTime > 0 && !m_rxFlowCompleteCb.IsNull()) {
            // 触发回调，记录 FCT
            m_rxFlowCompleteCb(rxQp, flowStartTime);
        } else {
            NS_LOG_WARN("Flow finished but missing start time tag!");
        }
        
        
    }

    // =========================================================
    // 阶段 F: 释放 Credit (流控)
    // =========================================================
    uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
    if (nic_idx < m_nic.size()) {
        Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
        if (dev) {
            // 释放 1 个 Flit 的空间 (注意这里是按 Flit 个数释放，不是字节)
            dev->ReleaseRxCredit(1); 
        }
    }

    return 0;


    // // 1. 获取 Flit 信息
    // FlitHeader fh;
    // p->PeekHeader(fh); // 先偷看，不移除，因为后面 HEAD/SINGLE 还要解析 IP 头
    // uint8_t type = fh.GetType(); // 0=HEAD, 1=BODY, 2=TAIL, 3=SINGLE

    // uint32_t flitLen = fh.GetPacketLen(); // Flit 占用的 Credit 数
    // // 【修改点 1】初始化 rxQp 为缓存的 QP (针对 BODY/TAIL)
    // Ptr<RdmaRxQueuePair> rxQp = m_currentRxQp;
    // uint32_t nodeId = m_node->GetId();

    // if (type == 0 || type == 3) {
        
    //     p->RemoveHeader(fh); // HEAD 或 SINGLE 包，移除 Flit 头
    //     p->PeekHeader(ch); // 偷看 CustomHeader，准备后续处理
    //     rxQp = GetRxQp(ch.dip, ch.sip, ch.udp.dport, ch.udp.sport, ch.udp.pg, true);
    //     if (rxQp == NULL) {
    //     uint64_t rxKey = GetRxQpKey(ch.sip, ch.udp.sport, ch.udp.dport, ch.udp.pg);
    //     if (akashic_RxQp.find(rxKey) != akashic_RxQp.end()) {
    //         // printf("[GetRxQPUDP] Akashic access: %u(%d) -> %u(%d)\n", this->m_node->GetId(),
    //         // ch.udp.dport, ch.sip, ch.udp.sport);
    //         return 1;  // just drop
    //     } else {
    //         printf("ERROR: UDP NIC cannot find the flow\n");
    //         exit(1);
    //     }
    // }
    // std::cout<<" Node "<<nodeId<<" receive packet "<<ch.udp.seq<<"  Expected Packet is  "<<rxQp->expected_seq<<std::endl;
    // if (ch.udp.seq != rxQp->expected_seq) { // 校验序号
    //     //报错，这是不正常的，说明机制有问题
    //     printf("ERROR: UDP NIC received out-of-order flit seq %u, expected %u\n",
    //            ch.udp.seq, rxQp->expected_seq);
    //            exit(1);

    // }
    // std::cout<<"rxQp->expected_seq要加的fh.GetPktTotalBytes()  =  "<<fh.GetPktTotalBytes()<<std::endl;
    // rxQp->expected_seq+=fh.GetPktTotalBytes(); // 更新期望的下一个序号
    // m_currentRxQp = rxQp;
    // }
    // //对于body和tail包，完全不用管，我只要头包对上了，就基本可以认为是正确的了
    //     // 在函数的最后，return 0 之前：
    
    // // =========================================================
    // // 阶段 B: 提取流信息 (从 Tag 中获取 Size 和 StartTime)
    // // =========================================================
    // // 只要 rxQp 存在，我们就尝试读取 Tag 来完善 rxQp 的信息
    // // 尤其是 m_size，如果之前没读到过，现在必须读到
    // double flowStartTime = 0;
    // if (rxQp) {
    //     // 1. 提取流的总大小
    //     if (rxQp->m_size == 0) {
    //         FlowIDNUMTag fint;
    //         if (p->PeekPacketTag(fint)) {
    //             rxQp->m_size = fint.GetFlowSize();
    //             // std::cout << "DEBUG: RxQP initialized size: " << rxQp->m_size << std::endl;
    //         }
    //     }

    //     // 2. 提取流的开始时间 (用于计算 FCT)
    //     FlowStatTag fst;
    //     if (p->PeekPacketTag(fst)) {
    //         // 注意：这里调用的是我们之前修改过的 getFlowStartTime()
    //         flowStartTime = fst.getFlowStartTime(); 
    //     }
    // }    




    //     if (rxQp) {
    //     uint32_t nic_idx = GetNicIdxOfRxQp(rxQp);
    //     // 加上越界检查更安全
    //     if (nic_idx < m_nic.size()) {
    //         Ptr<QbbNetDevice> dev = m_nic[nic_idx].dev;
    //         if (dev) {
    //             dev->ReleaseRxCredit(1);
    //         }
    //     }
    // }
    // return 0;
    
}

int RdmaHw::Receive(Ptr<Packet> p, CustomHeader &ch,uint32_t dev_idx) {
    // #if (SLB_DEBUG == true)
    //     std::cout << "[RdmaHw::Receive] Node(" << m_node->GetId() << ")," << PARSE_FIVE_TUPLE(ch)
    //     << "l3Prot:" << ch.l3Prot << ",at" << Simulator::Now() << std::endl;
    // #endif
    //if (ch.l3Prot == 0x11) {  // UDP
        return ReceiveUdp(p, ch, dev_idx);
    //} 
    
    return 0;
}



void RdmaHw::AddHeader(Ptr<Packet> p, uint16_t protocolNumber) {
    PppHeader ppp;
    ppp.SetProtocol(EtherToPpp(protocolNumber));
    p->AddHeader(ppp);
}

uint16_t RdmaHw::EtherToPpp(uint16_t proto) {
    switch (proto) {
        case 0x0800:
            return 0x0021;  // IPv4
        case 0x86DD:
            return 0x0057;  // IPv6
        default:
            NS_ASSERT_MSG(false, "PPP Protocol number not defined!");
    }
    return 0;
}



void RdmaHw::QpComplete(Ptr<RdmaQueuePair> qp) {
    NS_ASSERT(!m_qpCompleteCallback.IsNull());
    // if (m_cc_mode == 1) {
    //     Simulator::Cancel(qp->mlx.m_eventUpdateAlpha);
    //     Simulator::Cancel(qp->mlx.m_eventDecreaseRate);
    //     Simulator::Cancel(qp->mlx.m_rpTimer);
    // }
    //if (qp->m_retransmit.IsRunning()) qp->m_retransmit.Cancel();

    // This callback will log info. It also calls deletetion the rxQp on the receiver
    m_qpCompleteCallback(qp);
    // delete TxQueuePair
    DeleteQueuePair(qp);
}

void RdmaHw::SetLinkDown(Ptr<QbbNetDevice> dev) {
    printf("RdmaHw: node:%u a link down\n", m_node->GetId());
}

void RdmaHw::AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx) {
    uint32_t dip = dstAddr.Get();
    m_rtTable[dip].push_back(intf_idx);
}

void RdmaHw::ClearTable() { m_rtTable.clear(); }

void RdmaHw::RedistributeQp() {
    // clear old qpGrp
    for (uint32_t i = 0; i < m_nic.size(); i++) {
        if (m_nic[i].dev == NULL) continue;
        m_nic[i].qpGrp->Clear();
    }

    // redistribute qp
    for (auto &it : m_qpMap) {
        Ptr<RdmaQueuePair> qp = it.second;
        uint32_t nic_idx = GetNicIdxOfQp(qp);
        m_nic[nic_idx].qpGrp->AddQp(qp);
        // Notify Nic
        m_nic[nic_idx].dev->ReassignedQp(qp);
    }
}
//这个是很重要的一个函数，生成了一个包，给它加了头部，然后加了一些统计数据，然后更新qp的下一个发送指针
Ptr<Packet> RdmaHw::GetNxtPacket(Ptr<RdmaQueuePair> qp) {
    uint32_t payload_size = qp->GetBytesLeft();//这个是最重要的
    if (m_mtu < payload_size) {  // possibly last packet
        payload_size = m_mtu;
    }
    //std::cout<<"MTU="<<m_mtu<<std::endl;
    uint32_t seq = (uint32_t)qp->snd_nxt;//一定要确保snd_nxt这个正常更新
    bool proceed_snd_nxt = true;
    qp->stat.txTotalPkts += 1;
    qp->stat.txTotalBytes += payload_size;

    Ptr<Packet> p = Create<Packet>(payload_size);
    // add SeqTsHeader
    SeqTsHeader seqTs;
    seqTs.SetSeq(seq);
    seqTs.SetPG(qp->m_pg);
    p->AddHeader(seqTs);
    // add udp header
    UdpHeader udpHeader;
    udpHeader.SetDestinationPort(qp->dport);
    udpHeader.SetSourcePort(qp->sport);
    p->AddHeader(udpHeader);
    // add ipv4 header
    Ipv4Header ipHeader;
    ipHeader.SetSource(qp->sip);
    ipHeader.SetDestination(qp->dip);
    ipHeader.SetProtocol(0x11);
    ipHeader.SetPayloadSize(p->GetSize());
    ipHeader.SetTtl(64);
    ipHeader.SetTos(0);
    ipHeader.SetIdentification(qp->m_ipid);
    p->AddHeader(ipHeader);
    // add ppp header
    PppHeader ppp;
    ppp.SetProtocol(0x0021);  // EtherToPpp(0x800), see point-to-point-net-device.cc
    p->AddHeader(ppp);

    // attach Stat Tag
    uint8_t packet_pos = UINT8_MAX;
    {
        FlowIDNUMTag fint;
        if (!p->PeekPacketTag(fint)) {
            fint.SetId(qp->m_flow_id);
            fint.SetFlowSize(qp->m_size);
            p->AddPacketTag(fint);
        }
        FlowStatTag fst;
        uint64_t size = qp->m_size;
        if (!p->PeekPacketTag(fst)) {//统计数据这里估计得看看
            if (size < m_mtu && qp->snd_nxt + payload_size >= qp->m_size) {
                fst.SetType(FlowStatTag::FLOW_START_AND_END);
            } else if (qp->snd_nxt + payload_size >= qp->m_size) {
                fst.SetType(FlowStatTag::FLOW_END);
            } else if (qp->snd_nxt == 0) {
                fst.SetType(FlowStatTag::FLOW_START);
            } else {
                fst.SetType(FlowStatTag::FLOW_NOTEND);
            }
            packet_pos = fst.GetType();
            fst.setInitiatedTime(Simulator::Now().GetSeconds());
            // 2. 【新增】填入 QP 的开始时间 (流开始时间)
            // qp->startTime 是在 RdmaQueuePair 构造时记录的
            fst.setFlowStartTime(qp->startTime.GetSeconds());
            p->AddPacketTag(fst);
        }
    }

    

    // // update state
    if (proceed_snd_nxt) qp->snd_nxt += payload_size;//这里是正常更新的

    qp->m_ipid++;

    // return
    return p;
}

void RdmaHw::PktSent(Ptr<RdmaQueuePair> qp, Ptr<Packet> pkt, Time interframeGap) {
    qp->lastPktSize = pkt->GetSize();
    UpdateNextAvail(qp, interframeGap, pkt->GetSize());

    if (pkt) {
        CustomHeader ch(CustomHeader::L2_Header | CustomHeader::L3_Header |
                        CustomHeader::L4_Header);
        pkt->PeekHeader(ch);
#if (SLB_DEBUG == true)
        std::cout << "[RdmaHw::PktSent] Node(" << m_node->GetId() << ")," << PARSE_FIVE_TUPLE(ch)
                  << "l3Prot:" << ch.l3Prot << ",at" << Simulator::Now() << std::endl;
#endif
        RdmaHw::nAllPkts += 1;
        // if (ch.l3Prot == 0x11) {  // UDP
        //     // Update Timer
        //     if (qp->m_retransmit.IsRunning()) qp->m_retransmit.Cancel();
        //     qp->m_retransmit = Simulator::Schedule(qp->GetRto(m_mtu), &RdmaHw::HandleTimeout, this,
        //                                            qp, qp->GetRto(m_mtu));
        // } else if (ch.l3Prot == 0xFC || ch.l3Prot == 0xFD || ch.l3Prot == 0xFF) {  // ACK, NACK, CNP
        // } else if (ch.l3Prot == 0xFE) {                                            // PFC
        // }
    }
}


void RdmaHw::UpdateNextAvail(Ptr<RdmaQueuePair> qp, Time interframeGap, uint32_t pkt_size) {
    Time sendingTime;
    if (m_rateBound)
        sendingTime = interframeGap + Seconds(qp->m_rate.CalculateTxTime(pkt_size));
    else
        sendingTime = interframeGap + Seconds(qp->m_max_rate.CalculateTxTime(pkt_size));
    qp->m_nextAvail = Simulator::Now() + sendingTime;
}

void RdmaHw::ChangeRate(Ptr<RdmaQueuePair> qp, DataRate new_rate) {//感觉流控可能会用到
#if 1
    Time sendingTime = Seconds(qp->m_rate.CalculateTxTime(qp->lastPktSize));
    Time new_sendintTime = Seconds(new_rate.CalculateTxTime(qp->lastPktSize));
    qp->m_nextAvail = qp->m_nextAvail + new_sendintTime - sendingTime;
    // update nic's next avail event
    uint32_t nic_idx = GetNicIdxOfQp(qp);
    m_nic[nic_idx].dev->UpdateNextAvail(qp->m_nextAvail);
#endif

    // change to new rate
    qp->m_rate = new_rate;
}

}  // namespace ns3
