#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace exogs {

enum class Direction : uint8_t { TC = 0, TM = 1 };

enum class LinkState : uint8_t { Disconnected = 0, Connected = 1, Error = 2 };

struct PacketRecord {
  uint64_t ts_ns = 0;
  Direction dir = Direction::TC;
  uint16_t apid = 0;
  uint16_t seq = 0;
  uint8_t service = 0;
  uint8_t subservice = 0;
  std::string summary;
  std::vector<uint8_t> raw;
};

} // namespace exogs
