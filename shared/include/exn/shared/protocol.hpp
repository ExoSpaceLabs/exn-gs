#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace exn::proto {

enum class MsgType : uint16_t {
  Hello = 1,
  LinkState = 2,
  // Telemetry/raw packets received from the device link.
  PacketRx = 3,
  // Metadata notification for a packet forwarded to the device link.
  PacketTx = 4,
  Error = 255,

  Command = 10,
  Query = 11,
  QueryResult = 12,
  // Raw CCSDS Space Packet supplied by an IPC client for uplink routing.
  PacketSend = 13,
};

// Frames are encoded as:
// [u32_be len][u16_be type][payload...]
// where len = 2 + payload_bytes.
struct Frame {
  MsgType type;
  std::vector<uint8_t> payload;
};

// Packet metadata used by clients for monitoring. This is not the full packet.
struct PacketMeta {
  uint64_t ts_ns = 0;

  uint8_t ver = 0; // 3 bits
  uint8_t typ = 0; // 0=TM, 1=TC
  uint8_t shf = 0; // secondary-header flag

  uint16_t apid = 0; // 11 bits

  uint8_t seqf = 0; // 2 bits
  uint16_t seq = 0; // 14 bits

  uint16_t len = 0; // CCSDS Packet Data Length encoded field (N-1)

  // Best-effort PUS service fields when the secondary header matches the EXN profile.
  uint8_t svc = 0;
  uint8_t ssvc = 0;
};

std::vector<uint8_t> encode(const Frame& f);

// Tries to decode a single frame from buffer. If successful, consumes it.
bool try_decode(std::vector<uint8_t>& buffer, Frame& out);

std::vector<uint8_t> pack_string(const std::string& s);
bool unpack_string(const std::vector<uint8_t>& p, std::string& s);

// PacketMeta payload layout (big-endian):
// [u64 ts_ns]
// [u8 ver][u8 typ][u8 shf][u8 seqf]
// [u16 apid][u16 seq][u16 len]
// [u8 svc][u8 ssvc][u16 desc_len][desc bytes]
std::vector<uint8_t> pack_packet_meta(const PacketMeta& m, const std::string& desc);
bool unpack_packet_meta(const std::vector<uint8_t>& p, PacketMeta& m, std::string& desc);

} // namespace exn::proto
