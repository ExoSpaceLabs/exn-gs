#include "hardrtpp.hpp"

#include "exogs/shared/daemon/framer.hpp"

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace hardrt;
using boost::asio::ip::tcp;

// --------------------
// Minimal bounded ring queue (HardRT has no queue yet).
// Busy-waits with Task::sleep(1) to avoid burning CPU.
// --------------------
template <typename T, size_t N>
class RingQueue {
public:
  bool try_push(const T& v) {
    const auto h = head_.load(std::memory_order_relaxed);
    const auto t = tail_.load(std::memory_order_acquire);
    if (((h + 1) % N) == t) return false; // full
    buf_[h] = v;
    head_.store((h + 1) % N, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const auto t = tail_.load(std::memory_order_relaxed);
    const auto h = head_.load(std::memory_order_acquire);
    if (t == h) return false; // empty
    out = buf_[t];
    tail_.store((t + 1) % N, std::memory_order_release);
    return true;
  }

  void push_blocking(const T& v) {
    while (!try_push(v)) Task::sleep(1);
  }

  void pop_blocking(T& out) {
    while (!try_pop(out)) Task::sleep(1);
  }

private:
  std::array<T, N> buf_{};
  std::atomic<size_t> head_{0};
  std::atomic<size_t> tail_{0};
};

// --------------------
// Packet structs
// --------------------
struct TcMeta {
  uint16_t apid = 0;
  uint16_t seq  = 0;
  uint8_t  svc  = 0;
  uint8_t  ssvc = 0;
};

struct TmBytes {
  std::vector<uint8_t> bytes;
};

// --------------------
// Globals
// --------------------
static std::atomic<bool> g_verbose{false};
static std::atomic<bool> g_connected{false};

static RingQueue<TcMeta, 64> g_tc_q;
static RingQueue<TmBytes, 64> g_tm_q;

static std::shared_ptr<boost::asio::io_context> g_io;
static std::shared_ptr<tcp::acceptor> g_acc;
static std::shared_ptr<tcp::socket> g_sock;

// --------------------
// Forward declarations
// --------------------
static void start_session();
static void start_async_read(std::shared_ptr<exogs::daemon::CcsdsFramer> framer);
static void tc_dispatch_task(void*);
static void tm_tx_task(void*);

// --------------------
// CCSDS builder helpers (TM only, replies only)
// --------------------
static std::vector<uint8_t> build_tm(uint16_t apid, uint16_t seq, uint8_t svc, uint8_t ssvc) {
  const uint16_t ver = 0;
  const uint16_t typ = 0; // TM
  const uint16_t shf = 1;

  const uint16_t pktid =
      uint16_t((ver & 0x7) << 13) |
      uint16_t((typ & 0x1) << 12) |
      uint16_t((shf & 0x1) << 11) |
      uint16_t(apid & 0x07FF);

  const uint16_t seqf = 3; // standalone
  const uint16_t seqctl =
      uint16_t((seqf & 0x3) << 14) |
      uint16_t(seq & 0x3FFF);

  const uint16_t payload_sz = 2;           // svc + ssvc
  const uint16_t ccsds_len  = payload_sz - 1;

  std::vector<uint8_t> pkt;
  pkt.reserve(6 + payload_sz);

  pkt.push_back(uint8_t(pktid >> 8));
  pkt.push_back(uint8_t(pktid & 0xFF));
  pkt.push_back(uint8_t(seqctl >> 8));
  pkt.push_back(uint8_t(seqctl & 0xFF));
  pkt.push_back(uint8_t(ccsds_len >> 8));
  pkt.push_back(uint8_t(ccsds_len & 0xFF));
  pkt.push_back(svc);
  pkt.push_back(ssvc);
  return pkt;
}

// --------------------
// Transport: (re)arm accept and start a new session
// --------------------
static void start_session() {
  g_sock = std::make_shared<tcp::socket>(*g_io);
  auto framer = std::make_shared<exogs::daemon::CcsdsFramer>();

  framer->set_on_packet([](std::vector<uint8_t>&& pkt) {
    if (pkt.size() < 8) return;

    const uint16_t pktid = (uint16_t(pkt[0]) << 8) | pkt[1];
    const uint16_t typ   = (pktid >> 12) & 0x1;
    if (typ != 1) return; // only TC

    TcMeta tc;
    tc.apid = pktid & 0x07FFu;
    tc.seq  = ((uint16_t(pkt[2]) << 8) | pkt[3]) & 0x3FFFu;
    tc.svc  = pkt[6];
    tc.ssvc = pkt[7];

    g_tc_q.push_blocking(tc);
  });

  g_acc->async_accept(*g_sock, [framer](const boost::system::error_code& ec) {
    if (ec) {
      if (g_verbose.load()) {
        std::printf("[SIM] accept error: %s\n", ec.message().c_str());
        std::fflush(stdout);
      }
      start_session(); // retry accept
      return;
    }

    g_connected.store(true);
    if (g_verbose.load()) {
      std::printf("[SIM] client connected\n");
      std::fflush(stdout);
    }

    start_async_read(framer);
  });
}

static void start_async_read(std::shared_ptr<exogs::daemon::CcsdsFramer> framer) {
  static std::array<uint8_t, 2048> rxbuf{};

  g_sock->async_read_some(
      boost::asio::buffer(rxbuf),
      [framer](const boost::system::error_code& ec, std::size_t n) {
        if (ec) {
          g_connected.store(false);
          if (g_verbose.load()) {
            std::printf("[SIM] read error: %s\n", ec.message().c_str());
            std::fflush(stdout);
          }

          // Close and re-arm accept
          boost::system::error_code ignore;
          if (g_sock) {
            g_sock->shutdown(tcp::socket::shutdown_both, ignore);
            g_sock->close(ignore);
          }

          start_session();
          return;
        }

        framer->push_bytes(rxbuf.data(), n);
        start_async_read(framer);
      });
}

// --------------------
// HardRT tasks
// --------------------
static void tc_dispatch_task(void*) {
  uint16_t tm_seq = 100;

  for (;;) {
    TcMeta tc;
    g_tc_q.pop_blocking(tc);

    if (g_verbose.load()) {
      std::printf("[SIM RX] TC APID=%u SEQ=%u SVC=%u SSV=%u\n",
                  tc.apid, tc.seq, tc.svc, tc.ssvc);
      std::fflush(stdout);
    }

    // SIM responds only to requests.
    if (tc.svc == 1 && tc.ssvc == 1) {               // CONNECT
      g_tm_q.push_blocking({build_tm(tc.apid, tm_seq++, 1, 2)});
      continue;
    }
    if (tc.svc == 1 && tc.ssvc == 3) {               // DISCONNECT
      g_tm_q.push_blocking({build_tm(tc.apid, tm_seq++, 1, 4)});
      continue;
    }
    if (tc.svc == 2 && tc.ssvc == 1) {               // PING REQ
      g_tm_q.push_blocking({build_tm(tc.apid, tm_seq++, 2, 2)});
      continue;
    }
    if (tc.svc == 3 && tc.ssvc == 10) {              // HK_REQ
      g_tm_q.push_blocking({build_tm(tc.apid, tm_seq++, 3, 25)});
      continue;
    }
    if (tc.svc == 3 && tc.ssvc == 1) {               // HK_ENABLE -> ACK
      g_tm_q.push_blocking({build_tm(tc.apid, tm_seq++, 3, 2)});
      continue;
    }
    if (tc.svc == 3 && tc.ssvc == 2) {               // HK_DISABLE -> ACK
      g_tm_q.push_blocking({build_tm(tc.apid, tm_seq++, 3, 3)});
      continue;
    }

    g_tm_q.push_blocking({build_tm(tc.apid, tm_seq++, tc.svc, 0xFE)});
  }
}

static void tm_tx_task(void*) {
  for (;;) {
    TmBytes tm;
    g_tm_q.pop_blocking(tm);

    // Drop replies if not connected.
    if (!g_connected.load()) continue;

    if (g_verbose.load()) {
      if (tm.bytes.size() >= 8) {
        const uint16_t pktid = (uint16_t(tm.bytes[0]) << 8) | tm.bytes[1];
        const uint16_t apid  = pktid & 0x07FF;
        const uint16_t seq   = ((uint16_t(tm.bytes[2]) << 8) | tm.bytes[3]) & 0x3FFF;
        const uint8_t svc    = tm.bytes[6];
        const uint8_t ssvc   = tm.bytes[7];
        std::printf("[SIM TX] TM APID=%u SEQ=%u SVC=%u SSV=%u\n", apid, seq, svc, ssvc);
      } else {
        std::printf("[SIM TX] TM len=%zu\n", tm.bytes.size());
      }
      std::fflush(stdout);
    }

    auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(tm.bytes));
    g_io->post([bytes]() {
      if (!g_connected.load() || !g_sock) return;
      boost::asio::async_write(
          *g_sock, boost::asio::buffer(*bytes),
          [bytes](const boost::system::error_code&, std::size_t) {});
    });
  }
}

// --------------------
// main()
// --------------------
int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 9000;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "-v" || a == "--verbose") g_verbose = true;
  }

  std::printf("HardRT version: %s (0x%06X), port: %s (id=%d)\n",
              System::version_string(), System::version_u32(),
              System::port_name(), System::port_id());
  std::fflush(stdout);

  // HardRT init (no designated initializers; C++17 friendly)
  hrt_config_t cfg{};
  cfg.tick_hz = 1000;
  cfg.policy = HRT_SCHED_PRIORITY_RR;
  cfg.default_slice = 5;
  // Leave other fields defaulted by {}.

  if (System::init(cfg) != 0) {
    std::puts("HardRT init failed");
    return 1;
  }

  // TCP acceptor
  g_io = std::make_shared<boost::asio::io_context>();
  g_acc = std::make_shared<tcp::acceptor>(
      *g_io,
      tcp::endpoint(boost::asio::ip::make_address(host), port));

  std::printf("stm32_sim listening on %s:%u\n", host.c_str(), port);
  std::fflush(stdout);

  // Start accepting connections (non-blocking)
  start_session();

  // Run Asio in its own thread
  std::thread io_thread([&]() { g_io->run(); });

  // Create tasks (stack size + ids based on your example style)
  if (Task::create<2048, 0>(tc_dispatch_task, nullptr, HRT_PRIO1, 0) < 0)
    std::puts("create tc_dispatch_task failed");

  if (Task::create<2048, 1>(tm_tx_task, nullptr, HRT_PRIO0, 0) < 0)
    std::puts("create tm_tx_task failed");

  System::start();

  if (io_thread.joinable()) io_thread.join();
  return 0;
}
