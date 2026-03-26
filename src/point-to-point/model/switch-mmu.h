#ifndef SWITCH_MMU_H
#define SWITCH_MMU_H

#include <ns3/node.h>
#include <ns3/random-variable-stream.h>

#include <list>
#include <unordered_map>

#include "ns3/conga-routing.h"
#include "ns3/conweave-routing.h"
#include "ns3/letflow-routing.h"
#include "ns3/settings.h"
#define SWITCH_MMU_ALPHA 0.5        // 动态共享因子 (固定不变)

namespace ns3 {

class Packet;
class SwitchNode;
class SwitchMmu : public Object {
   public:
    static const unsigned pCnt = 128;

    void SetNode(SwitchNode* node);
    static TypeId GetTypeId(void);

    SwitchMmu(void);

    // 运行时配置接口：在 network-load-balance.cc 创建交换机后调用
    void ConfigPool(uint32_t poolSize, uint32_t minGuarantee);
    // =========================================================
    // 【核心架构】：基于 Alpha 动态阈值的共享大金库
    // =========================================================
    // 核心功能 1：申请空间（O(1) 动态阈值准入控制）
    int AllocateSpace(uint32_t portId);

    // 核心功能 2：存入 Payload
    void StorePacket(int index, Ptr<Packet> p);

    // 核心功能 3：零拷贝读取
    Ptr<Packet> ReadFlit(int index) const;

    // 核心功能 4：释放空间
    void FreeSpace(int index, uint32_t portId);

    uint32_t GetPoolFree() const;

    /*------------ Routing Objects (public, accessed by network-load-balance.cc) -------------*/
    CongaRouting m_congaRouting;
    LetflowRouting m_letflowRouting;
    ConWeaveRouting m_conweaveRouting;

    private:
    SwitchNode* m_node;

    // --- 物理共享池变量 ---
    uint32_t m_totalPoolSize;   // 物理大池子的总容量 
    uint32_t m_poolFree;//池子里面剩余空间         
    uint32_t m_minGuarantee;   // 每个端口的保底额度 (B_min)  
    double m_alpha;       // 动态共享因子 (Alpha)        

    std::vector<Ptr<Packet>> m_physicalSRAM; // 物理大池子：全场唯一存储真实数据包的地方
    std::queue<int> m_freeList;   // 空闲索引管家：存放可用的物理下标 (0 到 totalSize-1)           
    std::vector<uint32_t> m_portUsed;// 端口账本：记录每个端口当前总共占用了多少个物理块

    // --- 出端口信用变量 ---
    //uint32_t m_egressCredits[pCnt];
    //void ConfigNPort(uint32_t n_port);
   // bool CheckIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
    //bool CheckEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
    //void UpdateIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
    //void UpdateEgressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
    //void RemoveFromIngressAdmission(uint32_t port, uint32_t qIndex, uint32_t psize);
    // 【新增】Egress 空间管理接口
    // 检查是否有足够空间
    //bool CheckEgressAdmission(uint32_t port);
    // 占用空间 (Packet 进入 Egress Queue)
    //void UpdateEgressAdmission(uint32_t port);
    // 释放空间 (Packet 离开 Egress Queue)
    //void ReleaseEgressAdmission(uint32_t port);
    // 在 SwitchMmu 类 private 区域添加：
    // 记录每个入端口是否处于阻塞状态（队头包尝试发送失败，正在等待资源）
     //bool m_ingressBlocked[pCnt];
    //void ConfigBufferSize(uint32_t size);
    // config
    //uint32_t node_id;

    // 【新增】核心逻辑：Flit 离开通知
    // 返回值：如果整包发完了，返回需要释放的 Credit 数量；否则返回 0。
    //uint32_t CheckCreditRelease(uint32_t ingressPort, uint32_t qIndex, bool isTail);

    
    //出端口信用器,代表每个出端口还有多少credit
    //uint32_t m_egressCredits[pCnt];

    // 【核心入口】Device 收到包后调用它
    //void Input(Ptr<Packet> p, uint32_t inDev); 

    // 【核心调度】尝试把 inDev 队头的包发出去
    //void ArbitrateAndSend(uint32_t inDev);

    // 【唤醒机制】当出端口 outDev 空闲时调用
    //void NotifyOutputPortFree(uint32_t outDev);

    // 名单 A: 等待空间的人 (Space Waiters)
    // 含义：InDev 因为 OutDev 满了而被卡住
    //std::set<uint32_t> m_waitingForSpace[pCnt]; 

    // 名单 B: 等待锁的人 (Lock Waiters)
    // 含义：InDev 因为 OutDev 被别人占用而被卡住
    //std::set<uint32_t> m_waitingForLock[pCnt];






    // 【修改3】添加 RegisterWaitSpace 声明
     //void RegisterWaitSpace(uint32_t outDev, uint32_t inDev);
     //void RegisterWaitPort(uint32_t outDev, uint32_t inDev);
     //void ReleaseEgressAdmission(uint32_t outDev);
     //void WakeupIngress(uint32_t inDev);

    //uint32_t GetActivePortCnt(void) const { return m_activePortCnt; }
    // void SetActivePortCnt(uint32_t v) {
    //     m_activePortCnt = v;
    //     InitSwitch();
    // }


    //sharedmemory
    //uint32_t m_totalPoolSize;    // 物理大池子的总容量 (例如：1000个 Cell)
    //uint32_t m_poolFree;         // 当前全局大池子还剩多少个空位
    //uint32_t m_minGuarantee;     // 每个端口的保底额度 (B_min)
    //double m_alpha;              // 动态共享因子 (Alpha)

    // 物理大池子：全场唯一存储真实数据包的地方
    //std::vector<Ptr<Packet>> m_physicalSRAM; 
    
    // 空闲索引管家：存放可用的物理下标 (0 到 totalSize-1)
    //std::queue<int> m_freeList;              

    // 端口账本：记录每个端口当前总共占用了多少个物理块
    //std::vector<uint32_t> m_portUsed;
    // 核心功能 1：申请空间（执行 Alpha 动态阈值准入控制）
    // 成功返回物理下标 (index)，失败返回 -1
    //int AllocateSpace(int portId);

    // 核心功能 2：真实入库
   // void StorePacket(int index, Ptr<Packet> p);

    // 核心功能 3：读取发送（零拷贝，不会删除物理数据）
    //Ptr<Packet> ReadPacket(int index) const;

    // 核心功能 4：释放空间（收到 ACK 时调用）
    //void FreeSpace(int index, int portId);

    // 辅助功能：获取当前全局剩余空间（供日志或调试使用）
    //uint32_t GetPoolFree() const;

    
    /*------------ Routing Objects (moved to public) -------------*/
  //std::deque<Ptr<Packet>> m_ingressQueues[pCnt];
   //private:
    // 【修改4】添加 SwitchNode 指针
    //SwitchNode* m_node;
   // 【物理入端口队列】
    // 只有一个 VC，所以是一维数组：每个入端口一个队列
    //std::deque<Ptr<Packet>> m_ingressQueues[pCnt];
    
    // 轮询指针 (用于唤醒时的公平调度)
    //uint32_t m_rrPtr[pCnt]; // 每个出端口记录上次服务了谁

    // 【新增】Egress 端口占用记账
    //uint32_t m_egressUsedBytes[pCnt];
    // Egress 端口的静态阈值 (比如 100KB)
    //uint32_t m_egressLimitBytes;
    
    

    //unsigned m_activePortCnt{0};
    

};

} /* namespace ns3 */

#endif /* SWITCH_MMU_H */
