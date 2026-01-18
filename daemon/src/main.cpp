#include "exogs/daemon/app.hpp"
#include <iostream>

bool verbose = false;

static void print_usage(const char* exe) {
  std::cerr
  << "Usage: " << exe << " [--listen HOST:PORT] [--port SERIAL|tcp://HOST:PORT] [--baud N] [--logdir DIR]\n"
  << "  -v, --verbose        Print TC/TM packets to console\n"
  << "Example: " << exe << " --listen 127.0.0.1:7777 --port /dev/ttyACM0 --baud 115200\n"
  << "Example: " << exe << " --listen 127.0.0.1:7777 --port tcp://127.0.0.1:9000\n";

}

int main(int argc, char** argv) {
  exogs::daemon::AppConfig cfg;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* opt) {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << opt << "\n";
        print_usage(argv[0]);
        std::exit(2);
      }
      return std::string(argv[++i]);
    };

    if (a == "--listen") {
      auto v = need("--listen");
      auto pos = v.find(':');
      if (pos == std::string::npos) {
        std::cerr << "Invalid --listen format, expected HOST:PORT\n";
        return 2;
      }
      cfg.listen_host = v.substr(0, pos);
      cfg.listen_port = static_cast<uint16_t>(std::stoi(v.substr(pos + 1)));
    } else if (a == "--port") {
      cfg.serial_port = need("--port");
    } else if (a == "--baud") {
      cfg.baud = static_cast<uint32_t>(std::stoul(need("--baud")));
    } else if (a == "--logdir") {
      cfg.log_dir = need("--logdir");
    } else if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      return 0;
    } else if (a == "-v" || a == "--verbose") {
      verbose = true;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }
  cfg.verbose = verbose;
  exogs::daemon::App app(cfg);
  return app.run();
}
