#include "exogs/ui/ui_app.hpp"
#include "exogs/ui/ipc_client.hpp"
#include "exogs/shared/protocol.hpp"

#include <boost/asio.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <thread>

namespace exogs::ui {

static std::string status_color(const std::string& s) {
  (void)s;
  return s;
}

UiApp::UiApp(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

void UiApp::push_log(const std::string& s) {
  std::lock_guard<std::mutex> lk(mtx_);
  log_.push_back(s);
  while (log_.size() > 200) log_.pop_front();
}

int UiApp::run() {
  boost::asio::io_context io;
  IpcClient client(io, host_, port_);

  client.set_on_frame([this](const exogs::proto::Frame& f) {
    std::string s;
    if (!exogs::proto::unpack_string(f.payload, s)) return;

    switch (f.type) {
      case exogs::proto::MsgType::Hello:
        push_log("[hello] " + s);
        break;
      case exogs::proto::MsgType::LinkState: {
        std::lock_guard<std::mutex> lk(mtx_);
        link_line_ = s;
        break;
      }
      case exogs::proto::MsgType::PacketRx:
        push_log(s);
        break;
      default:
        break;
    }
  });

  client.start();

  std::thread io_thread([&]() { io.run(); });

  using namespace ftxui;
  auto screen = ScreenInteractive::Fullscreen();

  int selected_service = 0;

  auto renderer = Renderer([&] {
    std::string link;
    std::deque<std::string> log;

    {
      std::lock_guard<std::mutex> lk(mtx_);
      link = link_line_;
      log = log_;
    }

    // Services panel (static placeholders for now)
    std::vector<std::string> services = {
      "HK", "TIME", "EVENT", "MEM", "PAYLOAD", "MODE"
    };

    auto services_box = vbox({
      text("Services") | bold,
      separator(),
      vbox([
        &]{
          Elements rows;
          for (size_t i = 0; i < services.size(); ++i) {
            auto row = hbox({
              text(services[i]) | (i == (size_t)selected_service ? inverted : nothing),
              filler(),
              text("●") | color(Color::Green) // v0: always green
            });
            rows.push_back(row);
          }
          return rows;
        }())
    }) | border;

    Elements log_lines;
    for (auto it = log.rbegin(); it != log.rend() && log_lines.size() < 20; ++it) {
      log_lines.push_back(text(*it));
    }

    auto log_box = vbox({
      text("Received TCs") | bold,
      separator(),
      vbox(std::move(log_lines))
    }) | border;

    auto top = hbox({
      text("exo-gs") | bold,
      filler(),
      text("Link: " + link)
    });

    return vbox({
      top | border,
      hbox({
        services_box | size(WIDTH, LESS_THAN, 30),
        log_box
      })
    });
  });

  auto component = CatchEvent(renderer, [&](Event e) {
    if (e == Event::Character('q') || e == Event::Escape) {
      screen.ExitLoopClosure()();
      return true;
    }
    if (e == Event::Character('c')) {
      client.send_command("CONNECT");
      push_log("[cmd] CONNECT");
      return true;
    }
    if (e == Event::Character('d')) {
      client.send_command("DISCONNECT");
      push_log("[cmd] DISCONNECT");
      return true;
    }
    if (e == Event::Character('p')) {
      client.send_command("PING");
      push_log("[cmd] PING");
      return true;
    }
    return false;
  });

  screen.Loop(component);

  io.stop();
  if (io_thread.joinable()) io_thread.join();
  return 0;
}

} // namespace exogs::ui
