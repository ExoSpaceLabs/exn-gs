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

    // UI state updated from the IPC thread.
    std::mutex mtx_;
    std::string daemon_line_ = "CONNECTING";
    std::string device_line_ = "UNKNOWN";
    std::string notice_line_;
    std::unordered_map<int, UiServiceStatus> services_;

    std::deque<PacketRow> tc_rows_;
    std::deque<PacketRow> tm_rows_;

    void push_tc(const exn::proto::PacketMeta& m, const std::string& desc);
    void push_tm(const exn::proto::PacketMeta& m, const std::string& desc);
  };

} // namespace exn::ui
