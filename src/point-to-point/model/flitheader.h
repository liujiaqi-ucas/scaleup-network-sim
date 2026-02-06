/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#ifndef FLIT_HEADER_H
#define FLIT_HEADER_H

#include <stdint.h>
#include <iostream>
#include "ns3/header.h"
#include "ns3/buffer.h"

namespace ns3 {

/**
 * \ingroup ScaleUpNetwork
 * \brief 128-bit (16 Bytes) Flit Header for High-Performance Scale-up Networks
 *
 * Layout:
 * Word 0: Basic Control (Type, VC, Priority) + GBN SeqNum
 * Word 1: Piggyback ACK/NACK
 * Word 2: Piggyback Absolute Credit
 * Word 3: Packet Metadata (Reassembly ID) + Header CRC
 */
class FlitHeader : public Header
{
public:
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual void Print (std::ostream &os) const;
  virtual uint32_t GetSerializedSize (void) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);

  // --- Constructor & Destructor ---
  FlitHeader ();
  virtual ~FlitHeader ();

  // --- Setters (Word 0) ---
  void SetType (uint8_t type);      // 0=HEAD, 1=BODY, 2=TAIL, 3=SINGLE
  void SetVcId (uint8_t vc);        // Virtual Channel ID，固定是0
  void SetPriority (uint8_t prio);  // QoS Priority
  void SetSeqNum (uint16_t seq);    // GBN Sequence Number

  // --- Setters (Word 1) ---
  void SetAck (uint16_t ack_seq);   // Set Piggyback ACK
  void SetNack (uint16_t nack_seq); // Set Piggyback NACK
  
  // --- Setters (Word 2) ---
  void SetCredit (uint8_t vc, uint16_t limit); // Set Absolute Credit Limit

  // --- Setters (Word 3) ---
  void SetPacketId (uint16_t pid);  // ID for Packet Reassembly
  void SetHeaderCrc (uint16_t crc); // CRC for this 16B header

  // --- Getters (Word 0) ---
  uint8_t GetType () const;
  uint8_t GetVcId () const;
  uint8_t GetPriority () const;
  uint16_t GetSeqNum () const;

  // --- Getters (Word 1) ---
  bool HasAck () const;             // Is the Ack field valid?
  bool IsNack () const;             // Is it a NACK?
  uint16_t GetAckSeq () const;

  // --- Getters (Word 2) ---
  bool HasCredit () const;          // Is the Credit field valid?
  uint8_t GetCreditVc () const;
  uint16_t GetCreditLimit () const;

  // --- Getters (Word 3) ---
  uint16_t GetPacketId () const;
  uint16_t GetHeaderCrc () const;
 // --- 新增 Setters ---
  void SetPacketLen (uint16_t len); // 新增：设置包长度 (Flit数)

  // --- 新增 Getters ---
  uint16_t GetPacketLen () const;   // 新增：获取包长度
  // --- 新增：Word 2 的 Setter/Getter ---
  // 设置当前 Flit 的字节大小 (包含头部和载荷)
  void SetCurrentLen (uint16_t len); 
  uint16_t GetCurrentLen () const;
  void SetPktTotalBytes (uint32_t bytes);
  uint32_t GetPktTotalBytes ();
  void setcreditflag(uint32_t flag);
  void setackflag(uint32_t flag);
private:
  // 128-bit (16 Bytes) Hardware Layout
  // Using pragma pack to ensure no compiler padding
  #pragma pack(push, 1)
  struct RawData {
      // --- Word 0 (32 bits) ---
      uint32_t type       : 2;  // Flit Type
      uint32_t vc_id      : 4;  // VC ID (0-15)
      uint32_t priority   : 3;  // Priority (0-7)
      uint32_t reserved0  : 7;  // Padding
      uint32_t seq_num    : 16; // Seq Num

      // --- Word 1 (32 bits) ---
      uint32_t ack_valid  : 1;//ack是否有效
      uint32_t is_nack    : 1;//是nak吗
      
      
      uint32_t pkt_len    : 8;  // 包长度 (单位: Flit), 最大 255 Flits (64KB if 256B/Flit)
      uint32_t reserve  : 6;  // 
  
      
      uint32_t ack_seq    : 16;//ack的数值

      // --- Word 2 (32 bits) ---
      uint32_t cred_valid : 1;  // Credit 是否有效
      uint32_t cred_vc    : 4;  // Credit VC ID，这个也不用管
      uint32_t curr_len   : 11; // 当前 Flit 的字节长度 (最大 2047)，包括头部和载荷
      uint32_t cred_limit : 16; // 下游发给上游的，表示允许你发多少字节

      // --- Word 3 (32 bits) ---
      uint32_t packet_id  : 16; // 这个好像也暂时用不到
      uint32_t pkt_total_bytes : 16; // flit对应的包的原始的payload字节数，会在接收端用到,累加的时候用
  } m_data;
  #pragma pack(pop)
};

} // namespace ns3

#endif /* FLIT_HEADER_H */