#include "hardrtpp.hpp"

#include "exn/shared/ccsds.hpp"
#include "exn/shared/daemon/framer.hpp"
#include "exn/shared/exn_interfaces.h"

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace hardrt;
using boost::asio::ip::tcp;

template <typename T, size_t N>
class RingQueue {
public:
  bool try_push(const T& v) {
    const auto h = head_.load(std::memory_order_relaxed);
    const auto t = tail_.load(std::memory_order_acquire);
    if (((h + 1) % N) == t) return false;
    buf_[h] = v;
    head_.store((h + 1) % N, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) {
    const auto t = tail_.load(std::memory_order_relaxed);
    const auto h = head_.load(std::memory_order_acquire);
    if (t == h) return false;
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

struct TcMessage {
  exn::proto::PacketMeta meta;
  std::vector<uint8_t> application_data;
};

struct TmBytes {
  std::vector<uint8_t> bytes;
};

static std::atomic<bool> g_verbose{false};
static std::atomic<bool> g_connected{false};

static RingQueue<TcMessage, 64> g_tc_q;
static RingQueue<TmBytes, 64> g_tm_q;

static std::shared_ptr<boost::asio::io_context> g_io;
static std::shared_ptr<tcp::acceptor> g_acc;
static std::shared_ptr<tcp::socket> g_sock;

static void start_session();
static void start_async_read(std::shared_ptr<exn::daemon::CcsdsFramer> framer);
static void tc_dispatch_task(void*);
static void tm_tx_task(void*);

static std::vector<uint8_t> build_tm(const uint16_t apid,
                                     const uint16_t seq,
                                     const uint8_t svc,
                                     const uint8_t ssvc,
                                     const std::vector<uint8_t>& application_data = {}) {
  std::vector<uint8_t> packet;
  std::string error;
  if (!exn::spacepacket::build_pus_c_tm(
          apid, seq, svc, ssvc,
          seq, PUSID_GS, EXN_TIME_UNSYNCED,
          application_data, packet, error)) {
    std::printf("[EXN SIM TX] cannot build PUS-C TM: %s\n", error.c_str());
    std::fflush(stdout);
    return {};
  }
  return packet;
}

static std::vector<uint8_t> make_hk_report() {
  // HEARTBEAT TM[3,25]:
  // structure_id, sample_unix_ms, state, time_state, version, uptime,
  // last_error, restart/reset_count.
  std::vector<uint8_t> app(32U, 0U);
  exn_be_put_u16(app.data(), EXN_HK_HEARTBEAT);
  // sample_unix_ms remains 0 while this simulator is UNSYNCED.
  app[10] = 2U; // READY in the common logical-state vocabulary.
  app[11] = EXN_TIME_UNSYNCED;
  return app;
}

static std::vector<uint8_t> make_system_hk_report(const TcMessage& tc) {
  uint16_t transaction_id = 0U;
  uint8_t include_mask = 0U;
  if (tc.application_data.size() >= 3U) {
    transaction_id = exn_be_get_u16(tc.application_data.data());
    include_mask = tc.application_data[2];
  }

  const auto obc_hk = make_hk_report();
  const bool include_obc = (include_mask & 0x01U) != 0U;
  const bool missing_requested_endpoint = (include_mask & 0x1EU) != 0U;

  // transaction_id, present_mask, status, block_count
  std::vector<uint8_t> app(5U, 0U);
  exn_be_put_u16(app.data(), transaction_id);
  app[2] = include_obc ? 0x01U : 0x00U;
  app[3] = missing_requested_endpoint ? 1U : 0U; // PARTIAL if non-OBC nodes requested
  app[4] = include_obc ? 1U : 0U;

  if (include_obc) {
    const auto old_size = app.size();
    app.resize(old_size + 4U + obc_hk.size());
    exn_be_put_u16(app.data() + old_size, APID_OBC);
    exn_be_put_u16(app.data() + old_size + 2U,
                   static_cast<uint16_t>(obc_hk.size()));
    std::copy(obc_hk.begin(), obc_hk.end(), app.begin() + static_cast<std::ptrdiff_t>(old_size + 4U));
  }
  return app;
}

static void start_session() {
  g_sock = std::make_shared<tcp::socket>(*g_io);
  auto framer = std::make_shared<exn::daemon::CcsdsFramer>();

  if (g_verbose.load()) {
    std::printf("[EXN SIM] start session\n");
    std::fflush(stdout);
  }

  framer->set_on_packet([](std::vector<uint8_t>&& bytes) {
    if (g_verbose.load()) {
      std::printf("[EXN SIM RX] buffer length: %zu\n", bytes.size());
      std::printf("[EXN SIM RX] Data in: ");
      for (const auto b : bytes) std::printf("%hu ", static_cast<unsigned short>(b));
      std::printf("\n");
      std::fflush(stdout);
    }

    TcMessage tc;
    std::string error;
    if (!exn::spacepacket::parse_pus_c_tc(bytes, tc.meta, tc.application_data, error)) {
      if (g_verbose.load()) {
        std::printf("[EXN SIM RX] rejected packet: %s\n", error.c_str());
        std::fflush(stdout);
      }
      return;
    }

    if (tc.meta.apid != APID_OBC) {
      if (g_verbose.load()) {
        std::printf("[EXN SIM RX] TC not addressed to MCU: APID=%u\n", tc.meta.apid);
        std::fflush(stdout);
      }
      return;
    }

    if (g_verbose.load()) {
      std::printf("[EXN SIM RX] TC APID=%u SEQ=%u SVC=%u SSV=%u\n",
                  tc.meta.apid, tc.meta.seq, tc.meta.svc, tc.meta.ssvc);
      std::fflush(stdout);
    }

    g_tc_q.push_blocking(tc);
  });

  g_acc->async_accept(*g_sock, [framer](const boost::system::error_code& ec) {
    if (ec) {
      if (g_verbose.load()) {
        std::printf("[EXN SIM] accept error: %s\n", ec.message().c_str());
        std::fflush(stdout);
      }
      start_session();
      return;
    }

    g_connected.store(true);
    if (g_verbose.load()) {
      std::printf("[EXN SIM] client connected\n");
      std::fflush(stdout);
    }

    start_async_read(framer);
  });
}

static void start_async_read(std::shared_ptr<exn::daemon::CcsdsFramer> framer) {
  static std::array<uint8_t, 2048> rxbuf{};

  g_sock->async_read_some(
      boost::asio::buffer(rxbuf),
      [framer](const boost::system::error_code& ec, const std::size_t n) {
        if (ec) {
          g_connected.store(false);
          if (g_verbose.load()) {
            std::printf("[EXN SIM] read error: %s\n", ec.message().c_str());
            std::fflush(stdout);
          }

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

static void tc_dispatch_task(void*) {
  uint16_t tm_seq = 100U;

  for (;;) {
    TcMessage tc;
    g_tc_q.pop_blocking(tc);

    std::vector<uint8_t> reply;
    if (tc.meta.svc == SVC_HK && tc.meta.ssvc == SUB_HK_REQUEST) {
      reply = build_tm(APID_OBC, tm_seq++, SVC_HK, SUB_HK_REPORT, make_hk_report());
    } else if (tc.meta.svc == SVC_HK && tc.meta.ssvc == SUB_SYS_HK_REQUEST) {
      reply = build_tm(APID_OBC, tm_seq++, SVC_HK, SUB_SYS_HK_REPORT,
                       make_system_hk_report(tc));
    } else if (g_verbose.load()) {
      std::printf("[EXN SIM] PUS-C TC SVC=%u SSV=%u not implemented by simulator\n",
                  tc.meta.svc, tc.meta.ssvc);
      std::fflush(stdout);
    }

    tm_seq = static_cast<uint16_t>(tm_seq & 0x3FFFU);
    if (!reply.empty()) g_tm_q.push_blocking({std::move(reply)});
  }
}

static void tm_tx_task(void*) {
  for (;;) {
    TmBytes tm;
    g_tm_q.pop_blocking(tm);

    if (!g_connected.load()) {
      if (g_verbose.load()) {
        std::printf("[EXN SIM TX] not connected, dropping TM\n");
        std::fflush(stdout);
      }
      continue;
    }

    if (g_verbose.load()) {
      exn::proto::PacketMeta meta;
      std::string error;
      if (exn::spacepacket::decode_meta(tm.bytes, meta, error)) {
        std::printf("[EXN SIM TX] TM APID=%u SEQ=%u SVC=%u SSV=%u\n",
                    meta.apid, meta.seq, meta.svc, meta.ssvc);
      } else {
        std::printf("[EXN SIM TX] invalid TM: %s\n", error.c_str());
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

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 9000;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-v" || arg == "--verbose") g_verbose = true;
  }

  std::printf("HardRT version: %s (0x%06X), port: %s (id=%d)\n",
              System::version_string(), System::version_u32(),
              System::port_name(), System::port_id());
  std::fflush(stdout);

  hrt_config_t cfg{};
  cfg.tick_hz = 1000;
  cfg.policy = HRT_SCHED_PRIORITY_RR;
  cfg.default_slice = 5;

  if (System::init(cfg) != 0) {
    std::puts("HardRT init failed");
    return 1;
  }

  g_io = std::make_shared<boost::asio::io_context>();
  g_acc = std::make_shared<tcp::acceptor>(
      *g_io,
      tcp::endpoint(boost::asio::ip::make_address(host), port));

  std::printf("stm32_sim listening on %s:%u\n", host.c_str(), port);
  std::fflush(stdout);

  start_session();
  std::thread io_thread([&]() { g_io->run(); });

  if (Task::create<2048, 0>(tc_dispatch_task, nullptr, HRT_PRIO1, 0) < 0)
    std::puts("create tc_dispatch_task failed");

  if (Task::create<2048, 1>(tm_tx_task, nullptr, HRT_PRIO0, 0) < 0)
    std::puts("create tm_tx_task failed");

  System::start();

  if (io_thread.joinable()) io_thread.join();
  return 0;
}
