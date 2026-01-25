#include "exn/shared/daemon/framer.hpp"

namespace exn::daemon {

void CcsdsFramer::push_bytes(const uint8_t* data, size_t n) {
  if (!data || n == 0) return;
  acc_.insert(acc_.end(), data, data + n);

  while (true) {
    if (acc_.size() < 6) return;

    // CCSDS primary header length field at bytes 4..5.
    const uint16_t pkt_len = (uint16_t(acc_[4]) << 8) | uint16_t(acc_[5]);

    // Total size = 6 + (pkt_len + 1)
    const uint32_t total = 6u + (uint32_t(pkt_len) + 1u);

    // Basic sanity limits to avoid eating garbage forever.
    if (total < 7 || total > 65536) {
      framing_errors_++;
      // drop one byte and resync.
      acc_.erase(acc_.begin());
      continue;
    }

    if (acc_.size() < total) return;

    std::vector<uint8_t> pkt(acc_.begin(), acc_.begin() + total);
    acc_.erase(acc_.begin(), acc_.begin() + total);

    if (on_packet_) on_packet_(std::move(pkt));
  }
}

} // namespace exn::daemon
