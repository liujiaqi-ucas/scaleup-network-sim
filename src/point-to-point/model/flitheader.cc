/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include "flitheader.h"
#include "ns3/log.h"
#include "ns3/assert.h"
#include <cstring> // For std::memset

NS_LOG_COMPONENT_DEFINE ("FlitHeader");

namespace ns3 {

NS_OBJECT_ENSURE_REGISTERED (FlitHeader);

// ===========================================================================
// Constructor & Framework Methods
// ===========================================================================

FlitHeader::FlitHeader ()
{
  // Initialize all bits to 0 to prevent garbage in reserved fields
  std::memset (&m_data, 0, sizeof(m_data));
}

FlitHeader::~FlitHeader ()
{
}

TypeId
FlitHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::FlitHeader")
    .SetParent<Header> ()
    .AddConstructor<FlitHeader> ()
  ;
  return tid;
}

TypeId
FlitHeader::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

uint32_t
FlitHeader::GetSerializedSize (void) const
{
  // Always 16 bytes (128 bits) fixed size
  return 16;
}

void
FlitHeader::Print (std::ostream &os) const
{
  // Pretty print for trace files
  os << "Flit(";
  
  // Basic Info
  switch (m_data.type) {
    case 0: os << "HEAD"; break;
    case 1: os << "BODY"; break;
    case 2: os << "TAIL"; break;
    case 3: os << "SNGL"; break;
    default: os << "UNKN"; break;
  }
  
  os << " V" << (int)m_data.vc_id
     << " P" << (int)m_data.priority
     << " Seq=" << m_data.seq_num
     << " PktID=" << m_data.packet_id;

  // Piggyback ACK Info
  if (m_data.ack_valid) {
    os << " | " << (m_data.is_nack ? "NACK=" : "ACK=") << m_data.ack_seq;
  }

  // Piggyback Credit Info
  if (m_data.cred_valid) {
    os << " | Cred(V" << (int)m_data.cred_vc << ")=" << m_data.cred_limit;
  }
  // 新增打印 PacketLen
  if (m_data.type == 0 || m_data.type == 3) { // HEAD or SINGLE
      os << " Len=" << m_data.pkt_len;
  }
  os << ")";
}

// ===========================================================================
// Serialization / Deserialization
// ===========================================================================

void
FlitHeader::Serialize (Buffer::Iterator start) const
{
  // Treat the bit-field struct as an array of 4 uint32_t
  // This maintains the bit layout in memory when written to the buffer
  const uint32_t* buf = reinterpret_cast<const uint32_t*>(&m_data);

  start.WriteU32 (buf[0]); // Word 0
  start.WriteU32 (buf[1]); // Word 1
  start.WriteU32 (buf[2]); // Word 2
  start.WriteU32 (buf[3]); // Word 3
}

uint32_t
FlitHeader::Deserialize (Buffer::Iterator start)
{
  uint32_t* buf = reinterpret_cast<uint32_t*>(&m_data);

  buf[0] = start.ReadU32 ();
  buf[1] = start.ReadU32 ();
  buf[2] = start.ReadU32 ();
  buf[3] = start.ReadU32 ();

  return GetSerializedSize ();
}

// ===========================================================================
// Setters Implementation
// ===========================================================================

void 
FlitHeader::SetType (uint8_t type)
{
  m_data.type = type;
}

void 
FlitHeader::SetVcId (uint8_t vc)
{
  m_data.vc_id = vc;
}

void 
FlitHeader::SetPriority (uint8_t prio)
{
  m_data.priority = prio;
}

void 
FlitHeader::SetSeqNum (uint16_t seq)
{
  m_data.seq_num = seq;
}

void 
FlitHeader::SetAck (uint16_t ack_seq)
{
  m_data.ack_valid = 1;
  m_data.is_nack = 0;
  m_data.ack_seq = ack_seq;
}

void 
FlitHeader::SetNack (uint16_t nack_seq)
{
  m_data.ack_valid = 1;
  m_data.is_nack = 1;
  m_data.ack_seq = nack_seq;
}

void 
FlitHeader::SetCredit (uint8_t vc, uint16_t limit)
{
  m_data.cred_valid = 1;
  m_data.cred_vc = vc;
  m_data.cred_limit = limit;
}

void 
FlitHeader::SetPacketId (uint16_t pid)
{
  m_data.packet_id = pid;
}

// void 
// FlitHeader::SetHeaderCrc (uint16_t crc)
// {
//   m_data.header_crc = crc;
// }

// ===========================================================================
// Getters Implementation
// ===========================================================================

uint8_t 
FlitHeader::GetType () const
{
  return (uint8_t)m_data.type;
}

uint8_t 
FlitHeader::GetVcId () const
{
  return (uint8_t)m_data.vc_id;
}

uint8_t 
FlitHeader::GetPriority () const
{
  return (uint8_t)m_data.priority;
}

uint16_t 
FlitHeader::GetSeqNum () const
{
  return (uint16_t)m_data.seq_num;
}

bool 
FlitHeader::HasAck () const
{
  return (m_data.ack_valid == 1);
}

bool 
FlitHeader::IsNack () const
{
  return (m_data.is_nack == 1);
}

uint16_t 
FlitHeader::GetAckSeq () const
{
  return (uint16_t)m_data.ack_seq;
}

bool 
FlitHeader::HasCredit () const
{
  return (m_data.cred_valid == 1);
}

uint8_t 
FlitHeader::GetCreditVc () const
{
  return (uint8_t)m_data.cred_vc;
}

uint16_t 
FlitHeader::GetCreditLimit () const
{
  return (uint16_t)m_data.cred_limit;
}

uint16_t 
FlitHeader::GetPacketId () const
{
  return (uint16_t)m_data.packet_id;
}

// uint16_t 
// FlitHeader::GetHeaderCrc () const
// {
//   return (uint16_t)m_data.header_crc;
// }
void 
FlitHeader::SetPacketLen (uint16_t len)
{
  // 保护一下，别溢出 8 bits
  if (len > 255) len = 255; 
  m_data.pkt_len = len;
}

// ... Getters ...

uint16_t 
FlitHeader::GetPacketLen () const
{
  return (uint16_t)m_data.pkt_len;
}
void 
FlitHeader::SetCurrentLen (uint16_t len)
{
  // 11 bits 最大值为 2047
  if (len > 2047) 
    {
      NS_LOG_WARN ("Flit length " << len << " exceeds 11-bit limit (2047).");
      m_data.curr_len = 2047;
    }
  else
    {
      m_data.curr_len = len;
    }
}

uint16_t 
FlitHeader::GetCurrentLen () const
{
  return (uint16_t)m_data.curr_len;
}
uint32_t 
FlitHeader::GetPktTotalBytes ()
{
  return (uint32_t)m_data.pkt_total_bytes;  
}
void 
FlitHeader::SetPktTotalBytes (uint32_t bytes)
{
  m_data.pkt_total_bytes = bytes;
} // namespace ns3
void 
FlitHeader::setackflag(uint32_t flag){
  m_data.ack_valid=flag;
}
void 
FlitHeader::setcreditflag(uint32_t flag){
  m_data.cred_valid=flag;
}
}