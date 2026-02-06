/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
#include <iostream>
#include "ns3/abort.h"
#include "ns3/assert.h"
#include "ns3/log.h"
#include "sr-header.h"

NS_LOG_COMPONENT_DEFINE ("FlitHeader");

namespace ns3 {

// 注册 NS-3 对象系统
NS_OBJECT_ENSURE_REGISTERED (CommonHeader);
NS_OBJECT_ENSURE_REGISTERED (NackHeader);

// =================================================================
// Implementation: CommonHeader
// =================================================================

CommonHeader::CommonHeader () : m_type (FLIT_TYPE_DATA) {}

CommonHeader::~CommonHeader () {}

TypeId
CommonHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::CommonHeader")
    .SetParent<Header> ()
    .AddConstructor<CommonHeader> ();
  return tid;
}

TypeId
CommonHeader::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

void 
CommonHeader::Print (std::ostream &os) const
{
  os << "Type=" << (int)m_type;
  switch (m_type) {
    case FLIT_TYPE_DATA: os << "(DATA)"; break;
    case FLIT_TYPE_NACK: os << "(NACK)"; break;
    default: os << "(Unknown)"; break;
  }
}

uint32_t
CommonHeader::GetSerializedSize (void) const
{
  return 1; // 极小头部，只占 1 字节
}

void
CommonHeader::Serialize (Buffer::Iterator start) const
{
  // 写入 1 字节类型
  start.WriteU8 (m_type);
}

uint32_t
CommonHeader::Deserialize (Buffer::Iterator start)
{
  // 读取 1 字节类型
  m_type = start.ReadU8 ();
  return GetSerializedSize ();
}

void
CommonHeader::SetFlitType (FlitType type)
{
  m_type = (uint8_t)type;
}

FlitType
CommonHeader::GetFlitType (void) const
{
  return (FlitType)m_type;
}


// =================================================================
// Implementation: NackHeader
// =================================================================

NackHeader::NackHeader () 
  : m_firstMissing (0), 
    m_bitmap (0) 
{
}

NackHeader::~NackHeader () {}

TypeId
NackHeader::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::NackHeader")
    .SetParent<Header> ()
    .AddConstructor<NackHeader> ();
  return tid;
}

TypeId
NackHeader::GetInstanceTypeId (void) const
{
  return GetTypeId ();
}

void 
NackHeader::Print (std::ostream &os) const
{
  os << "NACK: FirstMissing=" << m_firstMissing 
     << " Bitmap=0x" << std::hex << m_bitmap << std::dec;
}

uint32_t
NackHeader::GetSerializedSize (void) const
{
  // 2字节 (FirstMissing) + 4字节 (Bitmap) = 6字节
  return 6;
}

void
NackHeader::Serialize (Buffer::Iterator start) const
{
  // 使用网络字节序写入
  start.WriteHtonU16 (m_firstMissing);
  start.WriteHtonU32 (m_bitmap);
}

uint32_t
NackHeader::Deserialize (Buffer::Iterator start)
{
  // 使用网络字节序读取
  m_firstMissing = start.ReadNtohU16 ();
  m_bitmap = start.ReadNtohU32 ();
  return GetSerializedSize ();
}

void
NackHeader::SetFirstMissing (uint16_t sn)
{
  m_firstMissing = sn;
}

uint16_t
NackHeader::GetFirstMissing (void) const
{
  return m_firstMissing;
}

void
NackHeader::SetBitmap (uint32_t bitmap)
{
  m_bitmap = bitmap;
}

uint32_t
NackHeader::GetBitmap (void) const
{
  return m_bitmap;
}

} // namespace ns3