#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "exogs/shared/types.hpp"

namespace exogs::daemon {

struct ServiceStatus {
  uint64_t last_seen_ns = 0;
};

struct StateSnapshot {
  exogs::LinkState link = exogs::LinkState::Disconnected;
  std::string link_detail;

  uint64_t rx_packets = 0;
  uint64_t rx_bytes = 0;
  uint64_t decode_errors = 0;
  uint64_t framing_errors = 0;

  std::unordered_map<uint8_t, ServiceStatus> services; // key: service
};

class StateStore {
public:
  explicit StateStore(size_t ring_capacity = 1000);

  void set_link(exogs::LinkState st, std::string detail);
  void on_packet(const exogs::PacketRecord& rec);
  void on_decode_error();
  void on_framing_error();

  StateSnapshot snapshot() const;
  std::deque<exogs::PacketRecord> last_packets(size_t n) const;

private:
  const size_t cap_;
  mutable std::mutex mtx_;

  StateSnapshot snap_;
  std::deque<exogs::PacketRecord> ring_;
};

} // namespace exogs::daemon
