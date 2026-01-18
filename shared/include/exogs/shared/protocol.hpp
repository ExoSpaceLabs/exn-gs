#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace exogs::proto {

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

std::vector<uint8_t> encode(const Frame& f);

// Tries to decode a single frame from buffer. If successful, consumes it.
bool try_decode(std::vector<uint8_t>& buffer, Frame& out);

// Simple string payload helpers for v0.
std::vector<uint8_t> pack_string(const std::string& s);
bool unpack_string(const std::vector<uint8_t>& p, std::string& s);

} // namespace exogs::proto
