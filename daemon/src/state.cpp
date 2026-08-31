#include "exn/daemon/state.hpp"

namespace exn::daemon {

StateStore::StateStore(size_t ring_capacity) : cap_(ring_capacity) {}

void StateStore::set_link(exn::LinkState st, std::string detail) {
  std::lock_guard<std::mutex> lk(mtx_);
  snap_.link = st;
  snap_.link_detail = std::move(detail);
}

void StateStore::on_packet(const exn::PacketRecord& rec) {
  std::lock_guard<std::mutex> lk(mtx_);
  snap_.rx_packets++;
  snap_.rx_bytes += rec.raw.size();
  snap_.services[rec.service].last_seen_ns = rec.ts_ns;

  ring_.push_back(rec);
  while (ring_.size() > cap_) ring_.pop_front();
}

void StateStore::on_tx(const size_t bytes) {
  std::lock_guard<std::mutex> lk(mtx_);
  snap_.tx_packets++;
  snap_.tx_bytes += bytes;
}

void StateStore::on_decode_error() {
  std::lock_guard<std::mutex> lk(mtx_);
  snap_.decode_errors++;
}

void StateStore::on_framing_error() {
  std::lock_guard<std::mutex> lk(mtx_);
  snap_.framing_errors++;
}

StateSnapshot StateStore::snapshot() const {
  std::lock_guard<std::mutex> lk(mtx_);
  return snap_;
}

std::deque<exn::PacketRecord> StateStore::last_packets(size_t n) const {
  std::lock_guard<std::mutex> lk(mtx_);
  if (n >= ring_.size()) return ring_;
  return std::deque<exn::PacketRecord>(ring_.end() - static_cast<long>(n), ring_.end());
}

} // namespace exn::daemon
