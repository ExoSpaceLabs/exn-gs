#include "exogs/ui/ui_app.hpp"
#include <iostream>

static void usage(const char* exe) {
  std::cerr << "Usage: " << exe << " [--connect HOST:PORT]\n";
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 7777;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--connect") {
      if (i + 1 >= argc) {
        usage(argv[0]);
        return 2;
      }
      std::string v = argv[++i];
      auto pos = v.find(':');
      if (pos == std::string::npos) {
        usage(argv[0]);
        return 2;
      }
      host = v.substr(0, pos);
      port = static_cast<uint16_t>(std::stoi(v.substr(pos + 1)));
    } else if (a == "-h" || a == "--help") {
      usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      usage(argv[0]);
      return 2;
    }
  }

  exogs::ui::UiApp app(host, port);
  return app.run();
}
