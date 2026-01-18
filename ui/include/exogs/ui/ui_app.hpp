#pragma once
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace exogs::ui {

struct UiServiceStatus {
  uint64_t last_seen_ns = 0;
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
  std::deque<std::string> log_;

  void push_log(const std::string& s);
};

} // namespace exogs::ui
