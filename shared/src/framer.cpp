#include "exn/shared/daemon/framer.hpp"

#include <CCSDSBuffer.h>

namespace exn::daemon {

void CcsdsFramer::push_bytes(const uint8_t* data, size_t n) {
  if (!data || n == 0) return;
  acc_.insert(acc_.end(), data, data + n);

  while (true) {
    if (acc_.size() < 6U) return;

    const auto declared_size = ccsds::buffer::declaredPacketSize(acc_.data(), acc_.size());
    if (!declared_size) {
      framing_errors_++;
      // Drop one byte and search for the next plausible version-0 primary header.
      acc_.erase(acc_.begin());
      continue;
    }

    const auto total = declared_size.value();
    if (total < 7U || total > 65542U) {
      framing_errors_++;
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
