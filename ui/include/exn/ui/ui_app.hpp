#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "exn/shared/protocol.hpp"

namespace exn::ui {

  struct UiServiceStatus {
    uint64_t last_seen_ns = 0;
  };

  struct PacketRow {
    exn::proto::PacketMeta m;
    std::string desc;
  };

  class UiApp {
  public:
    UiApp(std::string host, uint16_t port);
    int run();

  private:
    std::string host_;
    uint16_t port_;

    // UI state updated from IPC thread
    std::mutex mtx_;
    std::string link_line_ = "DISCONNECTED";
    std::unordered_map<int, UiServiceStatus> services_;

    std::deque<PacketRow> tc_rows_;
    std::deque<PacketRow> tm_rows_;

    void push_tc(const exn::proto::PacketMeta& m, const std::string& desc);
    void push_tm(const exn::proto::PacketMeta& m, const std::string& desc);
  };

} // namespace exn::ui
