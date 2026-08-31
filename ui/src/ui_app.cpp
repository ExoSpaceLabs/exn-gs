#include "exn/ui/ui_app.hpp"
#include "exn/ui/ipc_client.hpp"
#include "exn/shared/ccsds.hpp"
#include "exn/shared/exn_interfaces.h"
#include "exn/shared/protocol.hpp"
#include "exn/shared/time.hpp"

#include <boost/asio.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

namespace exn::ui {

static void trim_rows(std::deque<PacketRow>& q, size_t maxn) {
  while (q.size() > maxn) q.pop_front();
}

UiApp::UiApp(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

void UiApp::push_tx(const exn::proto::PacketMeta& m, const std::string& desc) {
  std::lock_guard<std::mutex> lk(mtx_);
  tx_rows_.push_back(PacketRow{m, desc});
  trim_rows(tx_rows_, 5000);
}

void UiApp::push_rx(const exn::proto::PacketMeta& m, const std::string& desc) {
  std::lock_guard<std::mutex> lk(mtx_);
  rx_rows_.push_back(PacketRow{m, desc});
  trim_rows(rx_rows_, 5000);
}

static std::string fmt_time_local(uint64_t ts_ns) {
  std::time_t t = static_cast<std::time_t>(ts_ns / 1000000000ull);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::setfill('0')
      << std::setw(2) << tm.tm_hour << ":"
      << std::setw(2) << tm.tm_min  << ":"
      << std::setw(2) << tm.tm_sec;
  return oss.str();
}

static std::string pad_left(const std::string& s, int w) {
  if (static_cast<int>(s.size()) >= w) return s;
  return std::string(w - static_cast<int>(s.size()), ' ') + s;
}

static std::string pad_right(const std::string& s, int w) {
  if (static_cast<int>(s.size()) >= w) return s.substr(0, static_cast<size_t>(w));
  return s + std::string(w - static_cast<int>(s.size()), ' ');
}

static std::string trunc_ellipsis(const std::string& s, int w) {
  if (w <= 0) return "";
  if (static_cast<int>(s.size()) <= w) return s;
  if (w <= 1) return s.substr(0, 1);
  return s.substr(0, static_cast<size_t>(w - 1)) + "…";
}

static std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

static std::string uppercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

static bool starts_with(const std::string& value, const char* prefix) {
  return value.rfind(prefix, 0) == 0;
}

struct Col { const char* name; int w; bool right; };

static std::string render_packet_table_text(const std::deque<PacketRow>& rows,
                                            int max_rows,
                                            int term_w) {
  const Col cols[] = {
    {"TS",   8, false},
    {"VER",  3, true},
    {"TYP",  3, true},
    {"SHF",  3, true},
    {"APID", 5, true},
    {"SEQF", 4, true},
    {"SEQ",  5, true},
    {"LEN",  5, true},
    {"SVC",  3, true},
    {"SSV",  4, true},
  };

  int base_w = 0;
  for (auto& c : cols) base_w += c.w + 1;

  int desc_w = std::max(10, term_w - base_w - 1);
  desc_w = std::min(desc_w, 80);

  std::ostringstream out;
  for (auto& c : cols) out << pad_right(c.name, c.w) << " ";
  out << pad_right("DESC", desc_w) << "\n";

  const int total_w = base_w + desc_w;
  out << std::string(std::max(0, total_w), '-') << "\n";

  int added = 0;
  for (auto it = rows.rbegin(); it != rows.rend() && added < max_rows; ++it, ++added) {
    const auto& m = it->m;
    std::string vals[10] = {
      fmt_time_local(m.ts_ns),
      std::to_string(m.ver),
      std::to_string(m.typ),
      std::to_string(m.shf),
      std::to_string(m.apid),
      std::to_string(m.seqf),
      std::to_string(m.seq),
      std::to_string(m.len),
      std::to_string(m.svc),
      std::to_string(m.ssvc),
    };

    for (int i = 0; i < 10; ++i) {
      const auto& c = cols[i];
      if (c.right) out << pad_left(vals[i], c.w) << " ";
      else         out << pad_right(vals[i], c.w) << " ";
    }
    out << pad_right(trunc_ellipsis(it->desc, desc_w), desc_w) << "\n";
  }

  return out.str();
}

static ftxui::Element preformatted_block(const std::string& s) {
  using namespace ftxui;
  Elements lines;
  size_t start = 0;
  while (start <= s.size()) {
    size_t end = s.find('\n', start);
    if (end == std::string::npos) end = s.size();
    lines.push_back(text(s.substr(start, end - start)));
    if (end == s.size()) break;
    start = end + 1;
  }
  return vbox(std::move(lines)) | frame;
}

int UiApp::run() {
  using namespace ftxui;

  boost::asio::io_context io;
  IpcClient client(io, host_, port_);
  boost::asio::steady_timer hk_timer(io);
  auto screen = ScreenInteractive::Fullscreen();

  std::atomic<bool> daemon_connected{false};
  std::atomic<bool> device_connected{false};
  std::atomic<bool> hk_enabled{false};
  uint16_t tc_sequence = 1U;
  uint16_t hk_transaction_id = 1U;

  auto set_notice = [&](const std::string& message) {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      notice_line_ = message;
    }
    screen.PostEvent(Event::Custom);
  };

  auto send_system_hk = [&]() {
    std::vector<uint8_t> app_data(5U, 0U);
    be_put_u16(app_data.data(), hk_transaction_id);
    app_data[2] = 0x07U; // MCU + PI + FPGA
    be_put_u16(app_data.data() + 3U, 0U); // default detail mask

    std::vector<uint8_t> packet;
    std::string error;
    if (!exn::spacepacket::build_pus_a_tc(APID_MCU,
                                           tc_sequence,
                                           SVC_HK,
                                           SUB_SYS_HK_REQ,
                                           SRCID_GS,
                                           app_data,
                                           packet,
                                           error)) {
      set_notice("ERROR: cannot build System HK TC: " + error);
      return;
    }

    client.send_packet(packet);
    tc_sequence = static_cast<uint16_t>((tc_sequence + 1U) & 0x3FFFU);
    hk_transaction_id = hk_transaction_id == 0xFFFFU
                          ? 1U
                          : static_cast<uint16_t>(hk_transaction_id + 1U);
  };

  std::function<void()> arm_hk;
  arm_hk = [&]() {
    hk_timer.expires_after(std::chrono::seconds(2));
    hk_timer.async_wait([&](const boost::system::error_code& ec) {
      if (ec) return;
      if (hk_enabled.load() && daemon_connected.load() && device_connected.load()) {
        send_system_hk();
      }
      arm_hk();
    });
  };

  client.set_on_connection_state([&](const bool connected, const std::string& detail) {
    daemon_connected.store(connected);
    if (!connected) device_connected.store(false);

    {
      std::lock_guard<std::mutex> lk(mtx_);
      daemon_line_ = connected
        ? "CONNECTED " + detail
        : "RECONNECTING " + detail;
      if (!connected) device_line_ = "UNKNOWN";
    }
    screen.PostEvent(Event::Custom);
  });

  client.set_on_frame([&](const exn::proto::Frame& f) {
    switch (f.type) {
      case exn::proto::MsgType::Hello: {
        std::string server;
        if (exn::proto::unpack_string(f.payload, server)) {
          set_notice("IPC handshake: " + server);
        }
        break;
      }
      case exn::proto::MsgType::LinkState: {
        std::string s;
        if (!exn::proto::unpack_string(f.payload, s)) return;
        device_connected.store(starts_with(s, "CONNECTED"));
        {
          std::lock_guard<std::mutex> lk(mtx_);
          device_line_ = s;
        }
        screen.PostEvent(Event::Custom);
        break;
      }
      case exn::proto::MsgType::PacketRx: {
        exn::proto::PacketMeta m;
        std::string desc;
        if (!exn::proto::unpack_packet_meta(f.payload, m, desc)) return;
        push_rx(m, desc);
        screen.PostEvent(Event::Custom);
        break;
      }
      case exn::proto::MsgType::PacketTx: {
        exn::proto::PacketMeta m;
        std::string desc;
        if (!exn::proto::unpack_packet_meta(f.payload, m, desc)) return;
        push_tx(m, desc);
        screen.PostEvent(Event::Custom);
        break;
      }
      case exn::proto::MsgType::QueryResult: {
        std::string s;
        if (exn::proto::unpack_string(f.payload, s)) set_notice(s);
        break;
      }
      case exn::proto::MsgType::Error: {
        std::string s;
        if (exn::proto::unpack_string(f.payload, s)) set_notice("ERROR: " + s);
        break;
      }
      default:
        break;
    }
  });

  client.start();
  arm_hk();
  std::thread io_thread([&]() { io.run(); });

  bool cmd_mode = false;
  bool help_mode = false;
  std::string cmd_input;

  auto cmd_input_comp = Input(&cmd_input, "type command…");

  auto main_renderer = Renderer([&] {
    std::string daemon;
    std::string device;
    std::string notice;
    std::deque<PacketRow> tx_rows;
    std::deque<PacketRow> rx_rows;

    {
      std::lock_guard<std::mutex> lk(mtx_);
      daemon = daemon_line_;
      device = device_line_;
      notice = notice_line_;
      tx_rows = tx_rows_;
      rx_rows = rx_rows_;
    }

    auto size = Terminal::Size();
    const int term_w = size.dimx;
    const int term_h = size.dimy;

    const int overhead = cmd_mode ? 15 : 11;
    const int available_h = std::max(6, term_h - overhead);
    const int table_rows = std::max(1, available_h - 3);

    const std::string tx_text = render_packet_table_text(tx_rows, table_rows, term_w / 2);
    const std::string rx_text = render_packet_table_text(rx_rows, table_rows, term_w / 2);

    const auto daemon_color = starts_with(daemon, "CONNECTED")
      ? Color::Green
      : (starts_with(daemon, "CONNECTING") || starts_with(daemon, "RECONNECTING")
           ? Color::Yellow
           : Color::Red);
    const auto device_color = starts_with(device, "CONNECTED")
      ? Color::Green
      : (device == "UNKNOWN" ? Color::Yellow : Color::Red);

    auto top_link_names = vbox({
      text("Daemon IPC: "),
      text("Device Link: ")
    });
    auto top_link_values = vbox({
      text(daemon),
      text(device)
    });
    auto top_link_status = vbox({
      text("● ") | color(daemon_color),
      text("● ") | color(device_color),
    });

    auto top = hbox({
      text("exn-gs") | bold,
      filler(),
      hbox({top_link_status, top_link_names, top_link_values})
    });

    const auto hk_color = hk_enabled.load() ? Color::Green : Color::Yellow;
    auto services_panel = hbox({
      text("Client tasks: ") | bold,
      text("HK ") | bold,
      text("●  ") | color(hk_color),
      text(hk_enabled.load() ? "enabled" : "disabled") | color(Color::GrayLight),
      text("  |  scheduled traffic runs only while the device link is connected")
        | color(Color::GrayLight)
    });

    auto tx_panel = vbox({
      text("Packets TX (GS -> device link)") | bold,
      separator(),
      preformatted_block(tx_text) | color(Color::GrayLight),
    }) | border | flex;

    auto rx_panel = vbox({
      text("Packets RX (device link -> GS)") | bold,
      separator(),
      preformatted_block(rx_text) | color(Color::GrayLight),
    }) | border | flex;

    Element notice_bar = notice.empty()
      ? text("Status: ready") | color(Color::GrayLight)
      : text("Status: " + notice) |
          color(starts_with(notice, "ERROR") ? Color::Red : Color::GrayLight);

    Element cmd_bar;
    if (cmd_mode) {
      cmd_bar = vbox({
        separator(),
        hbox({ text("CMD:/> ") | bold, cmd_input_comp->Render() }),
        separator(),
        text("Enter=send   Esc=close   command input owns all character keys")
          | color(Color::GrayLight),
      });
    } else {
      cmd_bar = text("Keys: c=CMD   h=HELP   q=QUIT") | color(Color::GrayLight);
    }

    auto base = vbox({
      top,
      services_panel,
      notice_bar,
      hbox({tx_panel, rx_panel}) | flex,
      cmd_bar,
    });

    if (!help_mode) return base;

    auto help = vbox({
      text("Help") | bold,
      separator(),
      text("UI keys:") | bold,
      text("  c   open command bar"),
      text("  h   toggle this help when the command bar is closed"),
      text("  q   quit when command/help is closed"),
      text("  Esc close command/help"),
      separator(),
      text("Daemon transport/operations:") | bold,
      text("  CONNECT       open the configured device transport"),
      text("  DISCONNECT    close the device transport"),
      text("  RECONNECT     cycle the device transport"),
      text("  PING          daemon IPC liveness check"),
      text("  STATUS        query current device-link state"),
      text("  STATS         query router transport counters"),
      separator(),
      text("UI-owned application tasks:") | bold,
      text("  HK_ENABLE     enable 2 s System HK requests"),
      text("  HK_DISABLE    disable periodic System HK"),
      text("  HK_REQ        send one System HK request"),
    }) | border | bgcolor(Color::Black) | color(Color::White);

    return dbox({
      base,
      help | clear_under | center,
    });
  });

  auto root = Container::Vertical({main_renderer});

  auto wrapped = CatchEvent(root, [&](Event e) {
    if (help_mode) {
      if (e == Event::Escape || e == Event::Character('h')) {
        help_mode = false;
        return true;
      }
      return true;
    }

    // Command mode owns character input. Global shortcuts must not intercept
    // command text such as HK_ENABLE/HK_DISABLE.
    if (cmd_mode) {
      if (e == Event::Escape) {
        cmd_mode = false;
        return true;
      }

      if (e == Event::Return) {
        const std::string command = uppercase(trim(cmd_input));
        if (!command.empty()) {
          if (command == "HK_ENABLE") {
            hk_enabled.store(true);
            set_notice("periodic System HK enabled");
          } else if (command == "HK_DISABLE") {
            hk_enabled.store(false);
            set_notice("periodic System HK disabled");
          } else if (command == "HK_REQ") {
            if (daemon_connected.load() && device_connected.load()) {
              boost::asio::post(io, send_system_hk);
              set_notice("System HK request queued");
            } else {
              set_notice("ERROR: HK_REQ requires daemon IPC and device link to be connected");
            }
          } else if (!daemon_connected.load()) {
            set_notice("ERROR: daemon IPC is not connected");
          } else {
            client.send_command(command);
            set_notice("sent daemon command: " + command);
          }
          cmd_input.clear();
        }
        return true;
      }

      return cmd_input_comp->OnEvent(e);
    }

    if (e == Event::Character('q') || e == Event::Escape) {
      screen.ExitLoopClosure()();
      return true;
    }

    if (e == Event::Character('h')) {
      help_mode = true;
      return true;
    }

    if (e == Event::Character('c')) {
      cmd_mode = true;
      return true;
    }

    return false;
  });

  screen.Loop(wrapped);

  hk_enabled.store(false);
  boost::system::error_code ignored;
  hk_timer.cancel(ignored);
  io.stop();
  if (io_thread.joinable()) io_thread.join();
  return 0;
}

} // namespace exn::ui
