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
#include <vector>


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
  // Fixed columns like htop. These do NOT collapse.
  // Total width without DESC: 8 + 1 + 3+1 +3+1 +3+1 +5+1 +4+1 +5+1 +5+1 +3+1 +4+1 = 55
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
  for (auto& c : cols) base_w += c.w + 1; // +1 space
  // base_w currently includes a trailing space; that's fine.

  // Leave at least 10 chars for DESC if possible.
  int desc_w = std::max(10, term_w - base_w - 1);
  // Hard clamp so it doesn't explode.
  desc_w = std::min(desc_w, 60);

  std::ostringstream out;

  // Header
  for (auto& c : cols) {
    out << pad_right(c.name, c.w) << " ";
  }
  out << pad_right("DESC", desc_w) << "\n";

  // Separator line
  int total_w = base_w + desc_w;
  out << std::string(std::max(0, total_w), '-') << "\n";

  // Rows (most recent first)
  int added = 0;
  for (auto it = rows.rbegin(); it != rows.rend() && added < max_rows; ++it, ++added) {
    const auto& m = it->m;
    const std::string ts = fmt_time_local(m.ts_ns);

    std::string vals[10] = {
      ts,
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
  // frame => clip content to the available box (prevents overflow and avoids wrapping)
  return vbox(std::move(lines)) | frame;
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

  std::atomic<bool> running{true};
  std::thread tick([&] {
    while (running.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      screen.PostEvent(Event::Custom);
    }
  });

  auto renderer = Renderer([&] {
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

    // Compute how many rows we can show.
    // Top bar + services + titles + separators + footer ~ 10-12 lines.
    const int overhead = 12;
    const int available_h = std::max(4, term_h - overhead);
    const int table_rows = std::max(1, available_h - 3); // header + separator + some padding

    // Render services in one line, no collapsing boxes.
    // We'll always show the labels; status dots can be improved later with real status.
    auto services_panel = hbox({
      text("Services: ") | bold,
      text("HK ") | bold, text("● ") | color(Color::Green), text("| ") | bold,
      text("TIME ") | bold, text("● ") | color(Color::Green), text("| ") | bold,
      text("EVENT ") | bold, text("● ") | color(Color::Green), text("| ") | bold,
      text("MEM ") | bold, text("● ") | color(Color::Yellow), text("| ") | bold,
      text("PAYLOAD ") | bold, text("● ") | color(Color::Red), text("| ") | bold,
      text("PI-CAM ") | bold, text("● ") | color(Color::Red), text("| ") | bold,
      text("FPGA-AI ") | bold, text("● ") | color(Color::Red), text("| ") | bold,
      text("MODE ") | bold, text("●") | color(Color::Green),
    }) | border;

    auto top = hbox({
      text("exo-gs") | bold,
      filler(),
      text("Link: " + link),
    }) | border;

    //auto services_panel = vbox({
    //  text("Services: ") | bold,
    //  separator(),
    //  services_line,
    //}) | border;

    // Build table text blocks
    const std::string tc_text = render_packet_table_text(tc_rows, table_rows, term_w / 2);
    const std::string tm_text = render_packet_table_text(tm_rows, table_rows, term_w / 2);

    auto tc_panel = vbox({
      text("TCs (sent from GS)") | bold,
      separator(),
      preformatted_block(tc_text) | ftxui::color(Color::GrayLight),
    }) | border | flex;

    auto tm_panel = vbox({
      text("TMs (received by GS)") | bold,
      separator(),
      preformatted_block(tm_text) | ftxui::color(Color::GrayLight),
    }) | border | flex;

    return vbox({
      top,
      services_panel,
      hbox({tc_panel, tm_panel}) | flex,
      text("Keys: c=CONNECT  d=DISCONNECT  p=PING  q=QUIT") | color(Color::GrayLight),
    });
  });

  auto component = CatchEvent(renderer, [&](Event e) {
    if (e == Event::Character('q') || e == Event::Escape) {
      screen.ExitLoopClosure()();
      return true;
    }
    if (e == Event::Character('c')) { client.send_command("CONNECT"); return true; }
    if (e == Event::Character('d')) { client.send_command("DISCONNECT"); return true; }
    if (e == Event::Character('p')) { client.send_command("PING"); return true; }
    return false;
  });

  screen.Loop(component);

  running.store(false);
  if (tick.joinable()) tick.join();

  io.stop();
  if (io_thread.joinable()) io_thread.join();
  return 0;
}

} // namespace exogs::ui
