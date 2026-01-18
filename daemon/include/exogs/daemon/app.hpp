#pragma once
#include <cstdint>
#include <string>

namespace exogs::daemon {

struct AppConfig {
  std::string listen_host = "127.0.0.1";
  uint16_t listen_port = 7777;

  std::string serial_port; // e.g. /dev/ttyACM0 or COM7
  uint32_t baud = 115200;

  std::string log_dir = "logs";
};

class App {
public:
  explicit App(AppConfig cfg);
  int run();

private:
  AppConfig cfg_;
};

} // namespace exogs::daemon
