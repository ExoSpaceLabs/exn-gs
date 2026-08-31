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

void UiApp::push_tc(const exn::proto::PacketMeta& m, const std::string& desc) {
  std::lock_guard<std::mutex> lk(mtx_);
  tc_rows_.push_back(PacketRow{m, desc});
  trim_rows(tc_rows_, 5000);
}

void UiApp::push_tm(const exn::proto::PacketMeta& m, const std::string& desc) {
  std::lock_guard<std::mutex> lk(mtx_);
  tm_rows_.push_back(PacketRow{m, desc});
  trim_rows(tm_rows_, 5000);
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
  if ((int)s.size() >= w) return s;
  return std::string(w - (int)s.size(), ' ') + s;
}
static std::string pad_right(const std::string& s, int w) {
  if ((int)s.size() >= w) return s.substr(0, w);
  return s + std::string(w - (int)s.size(), ' ');
}
static std::string trunc_ellipsis(const std::string& s, int w) {
  if (w <= 0) return "";
  if ((int)s.size() <= w) return s;
  if (w <= 1) return s.substr(0, 1);
  return s.substr(0, w - 1) + "…";
}

static std::string uppercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
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

  int total_w = base_w + desc_w;
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
  boost::asio::io_context io;
  IpcClient client(io, host_, port_);
  boost::asio::steady_timer hk_timer(io);
  std::atomic<bool> hk_enabled{false};
  uint16_t tc_sequence = 1U;
  uint16_t hk_transaction_id = 1U;

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
      std::cerr << "[ui] cannot build System HK TC: " << error << "\n";
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
      if (hk_enabled.load()) send_system_hk();
      arm_hk();
    });
  };

  client.set_on_frame([this](const exn::proto::Frame& f) {
    switch (f.type) {
      case exn::proto::MsgType::LinkState: {
        std::string s;
        if (!exn::proto::unpack_string(f.payload, s)) return;
        std::lock_guard<std::mutex> lk(mtx_);
        link_line_ = s;
        break;
      }
      case exn::proto::MsgType::PacketRx: {
        exn::proto::PacketMeta m;
        std::string desc;
        if (!exn::proto::unpack_packet_meta(f.payload, m, desc)) return;
        push_tm(m, desc);
        break;
      }
      case exn::proto::MsgType::PacketTx: {
        exn::proto::PacketMeta m;
        std::string desc;
        if (!exn::proto::unpack_packet_meta(f.payload, m, desc)) return;
        push_tc(m, desc);
        break;
      }
      case exn::proto::MsgType::Error: {
        std::string s;
        if (!exn::proto::unpack_string(f.payload, s)) return;
        std::lock_guard<std::mutex> lk(mtx_);
        link_line_ = "ERROR " + s;
        break;
      }
      default:
        break;
    }
  });

  client.start();
  arm_hk();
  std::thread io_thread([&]() { io.run(); });

  using namespace ftxui;
  auto screen = ScreenInteractive::Fullscreen();

  std::atomic<bool> running{true};
  std::thread tick([&] {
    while (running.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      screen.PostEvent(Event::Custom);
    }
  });

  bool cmd_mode = false;
  bool help_mode = false;
  std::string cmd_input;

  auto cmd_input_comp = Input(&cmd_input, "type command…");

  auto main_renderer = Renderer([&] {
    std::string link;
    std::deque<PacketRow> tc_rows;
    std::deque<PacketRow> tm_rows;

    {
      std::lock_guard<std::mutex> lk(mtx_);
      link = link_line_;
      tc_rows = tc_rows_;
      tm_rows = tm_rows_;
    }

    auto size = Terminal::Size();
    const int term_w = size.dimx;
    const int term_h = size.dimy;

    const int overhead = cmd_mode ? 14 : 10;
    const int available_h = std::max(6, term_h - overhead);
    const int table_rows = std::max(1, available_h - 3);

    const std::string tc_text = render_packet_table_text(tc_rows, table_rows, term_w / 2);
    const std::string tm_text = render_packet_table_text(tm_rows, table_rows, term_w / 2);

    auto topLinkName = vbox({
      text("Daemon Link: "),
      text("Device Link: ")
    });
    std::string link2 = "via daemon";
    auto topLinkValue = vbox({
      text(link),
      text(link2)
    });

    auto link1col = color(Color::Green);
    if (link == "DISCONNECTED" || link.rfind("ERROR", 0) == 0) {
      link1col = color(Color::Red);
    }

    auto topLinkStatus = vbox({
      text("● ") | link1col,
      text("● ") | link1col,
    });

    auto topLinks = hbox({
      topLinkStatus,
      topLinkName,
      topLinkValue
    });

    auto top = hbox({
      text("exn-gs") | bold,
      filler(),
      topLinks
    });

    auto hk_color = hk_enabled.load() ? color(Color::Green) : color(Color::Yellow);
    auto services_panel = hbox({
        text("Client tasks: ") | bold,
        text("HK ") | bold, text("●  ") | hk_color, text(" | ") | bold,
        text("TIME manual  |  EVENT RX  |  MEM manual  |  PAYLOAD manual  |  MODE manual")
          | color(Color::GrayLight)
    });

    auto tc_panel = vbox({
      text("TCs (sent from GS)") | bold,
      separator(),
      preformatted_block(tc_text) | color(Color::GrayLight),
    }) | border | flex;

    auto tm_panel = vbox({
      text("TMs (received by GS)") | bold,
      separator(),
      preformatted_block(tm_text) | color(Color::GrayLight),
    }) | border | flex;

    Element cmd_bar = filler();
    if (cmd_mode) {
      cmd_bar = vbox({
        separator(),
        hbox({ text("CMD:/> ") | bold, cmd_input_comp->Render() }),
        separator(),
        text("Enter=send   Esc=close   h=help") | color(Color::GrayLight),
      });
    } else {
      cmd_bar = text("Keys: c=CMD   h=HELP   q=QUIT") | color(Color::GrayLight);
    }

    auto base = vbox({
      top,
      services_panel,
      hbox({tc_panel, tm_panel}) | flex,
      cmd_bar,
    });

    if (!help_mode) return base;

    auto help = vbox({
      text("Help") | bold,
      separator(),
      text("UI keys:") | bold,
      text("  c   open CMD bar"),
      text("  h   toggle this help"),
      text("  q   quit (only when CMD/Help closed)"),
      text("  Esc close CMD/Help"),
      separator(),
      text("Daemon transport commands:") | bold,
      text("  CONNECT"),
      text("  DISCONNECT"),
      text("  PING          local daemon/link-state check"),
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

    if (!cmd_mode && (e == Event::Character('q') || e == Event::Escape)) {
      screen.ExitLoopClosure()();
      return true;
    }

    if (e == Event::Character('h')) {
      help_mode = true;
      return true;
    }

    if (e == Event::Character('c') && !cmd_mode) {
      cmd_mode = true;
      return true;
    }

    if (cmd_mode) {
      if (e == Event::Escape) {
        cmd_mode = false;
        return true;
      }
      if (e == Event::Return) {
        if (!cmd_input.empty()) {
          const std::string command = uppercase(cmd_input);
          if (command == "CONNECT") {
            client.send_command("CONNECT");
            hk_enabled.store(true);
          } else if (command == "DISCONNECT") {
            hk_enabled.store(false);
            client.send_command("DISCONNECT");
          } else if (command == "PING") {
            client.send_command("PING");
          } else if (command == "HK_ENABLE") {
            hk_enabled.store(true);
          } else if (command == "HK_DISABLE") {
            hk_enabled.store(false);
          } else if (command == "HK_REQ") {
            boost::asio::post(io, send_system_hk);
          } else {
            client.send_command(cmd_input);
          }
          cmd_input.clear();
        }
        return true;
      }
      return cmd_input_comp->OnEvent(e);
    }

    return false;
  });

  screen.Loop(wrapped);

  hk_enabled.store(false);
  running.store(false);
  if (tick.joinable()) tick.join();

  boost::system::error_code ignored;
  hk_timer.cancel(ignored);
  io.stop();
  if (io_thread.joinable()) io_thread.join();
  return 0;
}

} // namespace exn::ui
