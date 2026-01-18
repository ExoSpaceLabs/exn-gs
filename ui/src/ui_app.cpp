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

static void trim(std::deque<std::string>& q, size_t maxn) {
  while (q.size() > maxn) q.pop_front();
}

void UiApp::push_tc(const std::string& s) {
  std::lock_guard<std::mutex> lk(mtx_);
  tc_log_.push_back(s);
  trim(tc_log_, 200);
}

void UiApp::push_tm(const std::string& s) {
  std::lock_guard<std::mutex> lk(mtx_);
  tm_log_.push_back(s);
  trim(tm_log_, 200);
}

int UiApp::run() {
  boost::asio::io_context io;
  IpcClient client(io, host_, port_);

  client.set_on_frame([this](const exogs::proto::Frame& f) {
    std::string s;
    if (!exogs::proto::unpack_string(f.payload, s)) return;

    switch (f.type) {
      case exogs::proto::MsgType::Hello:
        push_tm("[hello] " + s);
        break;
      case exogs::proto::MsgType::LinkState: {
        std::lock_guard<std::mutex> lk(mtx_);
        link_line_ = s;
        break;
      }
      case exogs::proto::MsgType::PacketRx:
        push_tm(s);
        break;
      case exogs::proto::MsgType::PacketTx:
        push_tc(s);
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
    std::deque<std::string> tc_log;
    std::deque<std::string> tm_log;

    {
      std::lock_guard<std::mutex> lk(mtx_);
      link = link_line_;
      tc_log = tc_log_;
      tm_log = tm_log_;
    }

    // Services panel (static placeholders for now)
    std::vector<std::string> services = {
      "HK", "TIME", "EVENT", "MEM", "PAYLOAD", "MODE"
    };

    // Services as a row (htop-ish summary bar)
    Elements service_cells;
    for (size_t i = 0; i < services.size(); ++i) {
      auto cell = vbox({
        text(services[i]) | (i == (size_t)selected_service ? inverted : nothing),
        text("●") | color(Color::Green) // v0: always green
      }) | border;
      service_cells.push_back(cell);
    }
    auto services_row = vbox({
      text("Services") | bold,
      separator(),
      hbox(std::move(service_cells))
    }) | border;

    auto make_log_box = [&](const char* title, const std::deque<std::string>& src) {
      Elements lines;
      for (auto it = src.rbegin(); it != src.rend() && lines.size() < 18; ++it)
        lines.push_back(text(*it));
      return vbox({
        text(title) | bold,
        separator(),
        vbox(std::move(lines))
      }) | border;
    };

    auto tc_box = make_log_box("TCs (sent from GS)", tc_log);
    auto tm_box = make_log_box("TMs (received by GS)", tm_log);

    auto top = hbox({
      text("exo-gs") | bold,
      filler(),
      text("Link: " + link)
    });

    return vbox({
      top | border,
      services_row,
      hbox({
        tc_box,
        tm_box
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
      push_tc("[ui] CONNECT");
      return true;
    }
    if (e == Event::Character('d')) {
      client.send_command("DISCONNECT");
      push_tc("[ui] DISCONNECT");
      return true;
    }
    if (e == Event::Character('p')) {
      client.send_command("PING");
      push_tc("[ui] PING");
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
