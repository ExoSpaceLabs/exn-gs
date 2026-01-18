#include "exogs/ui/ui_app.hpp"
#include "exogs/ui/ipc_client.hpp"
#include "exogs/shared/protocol.hpp"
#include "exogs/shared/time.hpp"

#include <boost/asio.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <atomic>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

namespace exogs::ui {

static void trim_rows(std::deque<PacketRow>& q, size_t maxn) {
  while (q.size() > maxn) q.pop_front();
}

UiApp::UiApp(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

void UiApp::push_tc(const exogs::proto::PacketMeta& m, const std::string& desc) {
  std::lock_guard<std::mutex> lk(mtx_);
  tc_rows_.push_back(PacketRow{m, desc});
  trim_rows(tc_rows_, 5000);
}

void UiApp::push_tm(const exogs::proto::PacketMeta& m, const std::string& desc) {
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

// Render multi-line text WITHOUT wrapping (paragraph wraps).
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
  return vbox(std::move(lines)) | frame; // frame clips to available area
}

int UiApp::run() {
  boost::asio::io_context io;
  IpcClient client(io, host_, port_);

  client.set_on_frame([this](const exogs::proto::Frame& f) {
    switch (f.type) {
      case exogs::proto::MsgType::LinkState: {
        std::string s;
        if (!exogs::proto::unpack_string(f.payload, s)) return;
        std::lock_guard<std::mutex> lk(mtx_);
        link_line_ = s;
        break;
      }
      case exogs::proto::MsgType::PacketRx: {
        exogs::proto::PacketMeta m;
        std::string desc;
        if (!exogs::proto::unpack_packet_meta(f.payload, m, desc)) return;
        push_tm(m, desc);
        break;
      }
      case exogs::proto::MsgType::PacketTx: {
        exogs::proto::PacketMeta m;
        std::string desc;
        if (!exogs::proto::unpack_packet_meta(f.payload, m, desc)) return;
        push_tc(m, desc);
        break;
      }
      default:
        break;
    }
  });

  client.start();
  std::thread io_thread([&]() { io.run(); });

  using namespace ftxui;
  auto screen = ScreenInteractive::Fullscreen();

  // Refresh once per second
  std::atomic<bool> running{true};
  std::thread tick([&] {
    while (running.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      screen.PostEvent(Event::Custom);
    }
  });

  // CMD + help state
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

    // Leave room for top + services + footer/cmd bar.
    const int overhead = cmd_mode ? 14 : 10;
    const int available_h = std::max(6, term_h - overhead);
    const int table_rows = std::max(1, available_h - 3);

    const std::string tc_text = render_packet_table_text(tc_rows, table_rows, term_w / 2);
    const std::string tm_text = render_packet_table_text(tm_rows, table_rows, term_w / 2);

    auto top = hbox({
      text("exo-gs") | bold,
      filler(),
      text("Link: " + link),
    }) | border;

    auto services_panel = hbox({
        text("Services: ") | bold,
        text("HK ")|bold, text("●  ")|color(Color::Green), text(" | ") | bold,
        text("TIME ")|bold, text("●  ")|color(Color::Green), text(" | ") | bold,
        text("EVENT ")|bold, text("●  ")|color(Color::Green), text(" | ") | bold,
        text("MEM ")|bold, text("●  ")|color(Color::Yellow), text(" | ") | bold,
        text("PAYLOAD ")|bold, text("●  ")|color(Color::Red), text(" | ") | bold,
        text("MODE ")|bold, text("●")|color(Color::Green)
    }) | border;

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

    // Bottom bar: only show when CMD is open (as requested)
    Element cmd_bar = filler();
    if (cmd_mode) {
      cmd_bar = vbox({
        text("CMD") | bold,
        separator(),
        hbox({ text("> ") | bold, cmd_input_comp->Render() }) | border,
        text("Enter=send   Esc=close   h=help") | color(Color::GrayLight),
      }) | border;
    } else {
      cmd_bar = text("Keys: c=CMD   h=HELP   q=QUIT") | color(Color::GrayLight);
    }

    auto base = vbox({
      top,
      services_panel,
      hbox({tc_panel, tm_panel}) | flex,
      cmd_bar,
    });

    if (!help_mode)
      return base;

    // Help overlay (popup)
    auto help = vbox({
      text("Help") | bold,
      separator(),
      text("UI keys:") | bold,
      text("  c   open CMD bar"),
      text("  h   toggle this help"),
      text("  q   quit (only when CMD/Help closed)"),
      text("  Esc close CMD/Help"),
      separator(),
      text("Daemon commands (examples):") | bold,
      text("  CONNECT"),
      text("  DISCONNECT"),
      text("  PING"),
      text("  HK_ENABLE"),
      text("  HK_DISABLE"),
      text("  HK_REQ"),
    }) | border | bgcolor(Color::Black) | color(Color::White);

    return dbox({
      base,
      help | clear_under | center,
    });
  });

  // Root container owns focus between input and renderer.
  auto root = Container::Vertical({
    main_renderer
  });

  // Event handling over the whole app
  auto wrapped = CatchEvent(root, [&](Event e) {
    // Help has priority: Esc closes help first
    if (help_mode) {
      if (e == Event::Escape || e == Event::Character('h')) {
        help_mode = false;
        return true;
      }
      // don't let help steal typing etc.
      return true;
    }

    // Global quit only when not in CMD mode
    if (!cmd_mode && (e == Event::Character('q') || e == Event::Escape)) {
      screen.ExitLoopClosure()();
      return true;
    }

    // Toggle help
    if (e == Event::Character('h')) {
      help_mode = true;
      return true;
    }

    // Toggle CMD mode

    if (e == Event::Character('c') && !cmd_mode) {
      cmd_mode = true;
      return true;
    }

    // CMD mode behavior
    if (cmd_mode) {
      if (e == Event::Escape) {
        cmd_mode = false;
        return true;
      }
      if (e == Event::Return) {
        if (!cmd_input.empty()) {
          client.send_command(cmd_input);
          cmd_input.clear();
        }
        return true;
      }
      // forward everything else to the Input widget
      return cmd_input_comp->OnEvent(e);
    }

    return false;
  });

  screen.Loop(wrapped);

  running.store(false);
  if (tick.joinable()) tick.join();

  io.stop();
  if (io_thread.joinable()) io_thread.join();
  return 0;
}

} // namespace exogs::ui
