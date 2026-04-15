/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */


#include "ns3/header.h"
#include "ns3/enum.h"

namespace ns3 {

/**
 * \brief Flit 类型枚举
 * 用于 CommonHeader 中标识后续负载的类型
 */
enum FlitType {
  FLIT_TYPE_DATA   = 0, // 数据 Flit
  FLIT_TYPE_NACK   = 1, // NACK 控制 Flit
  FLIT_TYPE_ACK    = 2, // 累积 ACK (可选)
  FLIT_TYPE_CREDIT = 3, // 流控 Credit (可选)
  FLIT_TYPE_PFC    = 4  // PFC PAUSE/RESUME 控制帧
};

/**
 * =================================================================
 * Class: CommonHeader
 * 用途: 极小的公共头部，用于标识 Flit 类型
 * 大小: 1 字节 (uint8_t)
 * =================================================================
 */
class CommonHeader : public Header 
{
public:
  CommonHeader ();
  virtual ~CommonHeader ();

  // 设置/获取类型
  void SetFlitType (FlitType type);
  FlitType GetFlitType (void) const;

  // NS-3 Header 标准接口
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual void Print (std::ostream &os) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual uint32_t GetSerializedSize (void) const;

private:
  uint8_t m_type; // 存储 FlitType
};

/**
 * =================================================================
 * Class: NackHeader
 * 用途: NACK 控制包专用头部
 * 包含: First Missing SN (16bit) + Bitmap (32bit)
 * 大小: 2 + 4 = 6 字节
 * =================================================================
 */
class NackHeader : public Header 
{
public:
  NackHeader ();
  virtual ~NackHeader ();

  // 设置/获取 First Missing SN
  void SetFirstMissing (uint16_t sn);
  uint16_t GetFirstMissing (void) const;

  
  // 设置/获取 Bitmap (128bit，分高64位和低64位)
  void SetBitmap (uint64_t high, uint64_t low);
  uint64_t GetBitmapHigh (void) const;
  uint64_t GetBitmapLow (void) const;
// 按位操作辅助方法
  void SetBit (uint8_t pos);    // 设置第 pos 位 (0~127)
  void ClearBit (uint8_t pos);  // 清除第 pos 位
  bool TestBit (uint8_t pos) const; // 测试第 pos 位

  // NS-3 Header 标准接口
  static TypeId GetTypeId (void);
  virtual TypeId GetInstanceTypeId (void) const;
  virtual void Print (std::ostream &os) const;
  virtual void Serialize (Buffer::Iterator start) const;
  virtual uint32_t Deserialize (Buffer::Iterator start);
  virtual uint32_t GetSerializedSize (void) const;

private:
  uint16_t m_firstMissing; // 基准序列号
  //uint32_t m_bitmap;       // 相对位图
  uint64_t m_bitmapHigh;   // 位图高 64 位 (bit 64~127)
  uint64_t m_bitmapLow;    // 位图低 64 位 (bit 0~63)
};

} // namespace ns3

