#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace exn::proto {

  enum class MsgType : uint16_t {
    Hello = 1,
    LinkState = 2,
    // Telemetry received from the STM side (or demo generator).
    PacketRx = 3,
    // Telecommands sent by GS towards STM (or demo echo).
    PacketTx = 4,
    Error = 255,

    Command = 10,
    Query = 11,
    QueryResult = 12,
  };

  // Frames are encoded as:
  // [u32_be len][u16_be type][payload...]
  // where len = 2 + payload_bytes
  struct Frame {
    MsgType type;
    std::vector<uint8_t> payload;
  };

  // Packet metadata used by the UI tables.
  // This is *not* the full packet, just CCSDS primary header fields plus
  // optional service/subservice and a short description.
  struct PacketMeta {
    uint64_t ts_ns = 0;

    uint8_t ver = 0; // 3 bits
    uint8_t typ = 0; // 0=TM, 1=TC
    uint8_t shf = 0; // secondary header flag

    uint16_t apid = 0; // 11 bits

    uint8_t seqf = 0;  // 2 bits
    uint16_t seq = 0;  // 14 bits

    uint16_t len = 0;  // CCSDS packet length field

    // Optional fields derived from secondary header (if present / simulated)
    uint8_t svc = 0;
    uint8_t ssvc = 0;
  };

  std::vector<uint8_t> encode(const Frame& f);

  // Tries to decode a single frame from buffer. If successful, consumes it.
  bool try_decode(std::vector<uint8_t>& buffer, Frame& out);

  // Simple string payload helpers for v0.
  std::vector<uint8_t> pack_string(const std::string& s);
  bool unpack_string(const std::vector<uint8_t>& p, std::string& s);

  // PacketMeta helpers.
  // Payload layout (big-endian):
  // [u64 ts_ns]
  // [u8 ver][u8 typ][u8 shf][u8 seqf]
  // [u16 apid][u16 seq][u16 len]
  // [u8 svc][u8 ssvc][u16 desc_len][desc bytes]
  std::vector<uint8_t> pack_packet_meta(const PacketMeta& m, const std::string& desc);
  bool unpack_packet_meta(const std::vector<uint8_t>& p, PacketMeta& m, std::string& desc);

} // namespace exn::proto
