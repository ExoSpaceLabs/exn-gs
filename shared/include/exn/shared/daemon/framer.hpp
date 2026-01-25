#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace exn::daemon {

// CCSDS Space Packet framer for primary header length field.
// It extracts full packets from a byte stream using the Packet Length field.
class CcsdsFramer {
public:
  using OnPacket = std::function<void(std::vector<uint8_t>&&)>;

  void push_bytes(const uint8_t* data, size_t n);
  void set_on_packet(OnPacket cb) { on_packet_ = std::move(cb); }

  // stats
  uint64_t framing_errors() const { return framing_errors_; }

private:
  std::vector<uint8_t> acc_;
  OnPacket on_packet_;
  uint64_t framing_errors_ = 0;
};

} // namespace exn::daemon
