#include <CCSDSHeader.h>
#include <CCSDSPacket.h>
#include <PusServices.h>
#include "hardrtpp.hpp"

#include "exn/shared/daemon/framer.hpp"

#include "exn/shared/exn_interfaces.h"

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
static void start_async_read(std::shared_ptr<exn::daemon::CcsdsFramer> framer);
static void tc_dispatch_task(void*);
static void tm_tx_task(void*);

// --------------------
// CCSDS builder helpers (TM only, replies only)
// --------------------
static std::vector<uint8_t> build_tm(uint16_t apid, uint16_t seq, uint8_t svc, uint8_t ssvc) {
  CCSDS::Packet pkt;
  pkt.setUpdatePacketEnable(true);

  auto& header = pkt.getPrimaryHeader();
  header.setVersionNumber(0);
  header.setType(0); // TM
  header.setDataFieldHeaderFlag(1);
  header.setAPID(apid);
  header.setSequenceFlags(CCSDS::ESequenceFlag::UNSEGMENTED);
  header.setSequenceCount(seq);

  // Use PusA for TM (Service Type, Subtype, Source ID, etc.)
  // PusA(version, serviceType, serviceSubtype, sourceID, dataLength)
  auto pus_hdr = std::make_shared<PusA>(1, svc, ssvc, SRCID_MCU, 0);
  pkt.setDataFieldHeader(pus_hdr);

  return pkt.serialize();
}

// --------------------
// Transport: (re)arm accept and start a new session
// --------------------
static void start_session() {
  g_sock = std::make_shared<tcp::socket>(*g_io);
  auto framer = std::make_shared<exn::daemon::CcsdsFramer>();
  if (g_verbose.load()) {
    std::printf("[EXN SIM] start session\n");
    std::fflush(stdout);
  }

  framer->set_on_packet([](std::vector<uint8_t>&& bytes) {
    CCSDS::Packet pkt;
    pkt.setUpdatePacketEnable(false);

    const std::vector<uint8_t>& data{bytes};
    // Register PUS types for deserialization
    if (g_verbose.load()) {
      std::printf("[EXN SIM RX] buffer length: %lu \n",data.size());

      std::printf("[EXN SIM RX] Data in: ");
      for (const auto b : data) {
        printf("%hu ", static_cast<uint8_t>(b));
      }
      printf("\n");
      std::fflush(stdout);
    }

    if (const auto exp = pkt.deserialize(data); !exp.has_value()) {
      // Fallback if not PusA or something else
      if (g_verbose.load()) {
        std::printf("[EXN SIM RX] CCSDS Packet Deserialize fail PUS-A: %s\n", exp.error().message().c_str());
        std::fflush(stdout);
      }
      if (const auto expFallback = pkt.deserialize(data); !expFallback.has_value()) {
        if (g_verbose.load()) {
          std::printf("[EXN SIM RX] CCSDS Packet Deserialize fail FALLBACK: %s\n", expFallback.error().message().c_str());
          std::fflush(stdout);
        }
        return;
      }
    }
    std::printf("[EXN SIM RX] Packet header: %lu \n",pkt.getPrimaryHeader64bit());
    std::fflush(stdout);
    auto& header = pkt.getPrimaryHeader();
    if (pkt.getPrimaryHeader().getType() != 1) {
      if (g_verbose.load()) {
        std::printf("[EXN SIM RX] CCSDS Header is not type 1 (TC), received %u\n", header.getType());
        std::fflush(stdout);
      }
      return;
    }
    // only TC

    TcMeta tc;
    tc.apid = header.getAPID();
    tc.seq  = header.getSequenceCount();

    if (header.getDataFieldHeaderFlag()) {
      if (auto sec_hdr = pkt.getDataField().getSecondaryHeader()) {
        if (sec_hdr->getType() == "PusA") {
          const auto pus_a = std::static_pointer_cast<PusA>(sec_hdr);
          tc.svc = pus_a->getServiceType();
          tc.ssvc = pus_a->getServiceSubtype();
        }
      }
    }
    if (g_verbose.load()) {
      std::printf("[EXN SIM] push message in queue\n");
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
      start_session(); // retry accept
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
      [framer](const boost::system::error_code& ec, std::size_t n) {
        if (ec) {
          g_connected.store(false);
          if (g_verbose.load()) {
            std::printf("[EXN SIM] read error: %s\n", ec.message().c_str());
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

  if (g_verbose.load()) {
    std::printf("[EXN SIM RX] TC dispatch Task\n");
    std::fflush(stdout);
  }

  for (;;) {
    TcMeta tc;
    if (g_verbose.load()) {
      std::printf("[EXN SIM] pop message from queue\n");
      std::fflush(stdout);
    }
    g_tc_q.pop_blocking(tc);

    if (g_verbose.load()) {
      std::printf("[EXN SIM RX] TC APID=%u SEQ=%u SVC=%u SSV=%u\n",
                  tc.apid, tc.seq, tc.svc, tc.ssvc);
      std::fflush(stdout);
    }

    // SIM responds only to requests.
    if (tc.svc == SVC_TIME && tc.ssvc == SUB_TIME_SET) {               // PING / TIME_SET
      g_tm_q.push_blocking({build_tm(APID_GS, tm_seq++, SVC_TIME, SUB_TIME_REPORT)});
      continue;
    }
    if (tc.svc == SVC_HK && tc.ssvc == SUB_HK_REQ) {                  // HK_REQ
      g_tm_q.push_blocking({build_tm(APID_GS, tm_seq++, SVC_HK, SUB_HK_REPORT)});
      continue;
    }
    if (tc.svc == SVC_HK && tc.ssvc == SUB_SYS_HK_REQ) {              // SYS_HK_REQ
      g_tm_q.push_blocking({build_tm(APID_GS, tm_seq++, SVC_HK, SUB_SYS_HK_REPORT)});
      continue;
    }

    g_tm_q.push_blocking({build_tm(APID_GS, tm_seq++, tc.svc, 0xFE)});
  }
}

static void tm_tx_task(void*) {
  for (;;) {
    if (g_verbose.load()) {
      std::printf("[EXN SIM TX] TM Task\n");
      std::fflush(stdout);
    }
    TmBytes tm;
    g_tm_q.pop_blocking(tm);

    // Drop replies if not connected.
    if (!g_connected.load()) {
      if (g_verbose.load()) {
        std::printf("[EXN SIM TX] Error: Not connected, Dropping bytes.\n");
        std::fflush(stdout);
      }
      continue;
    }
    CCSDS::Packet pkt;
    if (pkt.deserialize(tm.bytes, "PusA").has_value()) {
      auto& header = pkt.getPrimaryHeader();
      const uint16_t apid = header.getAPID();
      const uint16_t seq  = header.getSequenceCount();
      uint8_t svc = 0, ssvc = 0;
      if (header.getDataFieldHeaderFlag()) {
        auto sec_hdr = pkt.getDataField().getSecondaryHeader();
        if (sec_hdr && sec_hdr->getType() == "PusA") {
          auto pus_a = std::static_pointer_cast<PusA>(sec_hdr);
          svc = pus_a->getServiceType();
          ssvc = pus_a->getServiceSubtype();
        } else {
          std::printf("[EXN SIM TX] TM len=%zu (invalid CCSDS)\n", tm.bytes.size());
          std::fflush(stdout);
        }
      }
    if (g_verbose.load()) {
        std::printf("[EXN SIM TX] TM APID=%u SEQ=%u SVC=%u SSV=%u\n", apid, seq, svc, ssvc);
        std::fflush(stdout);
      }
    }

    auto bytes = std::make_shared<std::vector<uint8_t>>(std::move(tm.bytes));
    g_io->post([bytes]() {
      if (!g_connected.load() || !g_sock) {
        if (g_verbose.load()) {
          std::printf("[EXN SIM TX] Error: Not connected / Sock closed.\n");
          std::fflush(stdout);
        }
        return;
      }
      if (g_verbose.load()) {
        std::printf("[EXN SIM TX] DBG: Packet buffer data size: %lu\n",bytes->size());
        std::fflush(stdout);
      }
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
