#include "exogs/shared/daemon/framer.hpp"
#include "exogs/shared/time.hpp"

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using boost::asio::ip::tcp;

static void print_usage(const char* exe) {
  std::cerr
    << "Usage: " << exe << " [--listen HOST:PORT] [--hk-period SEC] [-v|--verbose]\n"
    << "Example: " << exe << " --listen 127.0.0.1:9000 --hk-period 2 -v\n";
}

static void print_pkt(const char* tag,
                      const char* kind,
                      uint16_t apid,
                      uint16_t seq,
                      uint8_t svc,
                      uint8_t ssvc) {
  std::cout
    << tag << " " << kind
    << " APID=" << apid
    << " SEQ=" << seq
    << " SVC=" << int(svc)
    << " SSV=" << int(ssvc)
    << "\n";
}

static std::vector<uint8_t> build_ccsds(bool is_tc, uint16_t apid, uint16_t seq,
                                       uint8_t svc, uint8_t ssvc,
                                       const std::vector<uint8_t>& extra = {}) {
  const uint16_t ver = 0;
  const uint16_t typ = is_tc ? 1 : 0;
  const uint16_t shf = 1;

  const uint16_t pktid = uint16_t((ver & 0x7) << 13) | uint16_t((typ & 0x1) << 12) |
                         uint16_t((shf & 0x1) << 11) | uint16_t(apid & 0x07FF);

  const uint16_t seqf = 3; // standalone
  const uint16_t seqctl = uint16_t((seqf & 0x3) << 14) | uint16_t(seq & 0x3FFF);

  const size_t payload_sz = 2 + extra.size(); // svc + ssvc + extra
  const uint16_t ccsds_len = uint16_t(payload_sz - 1); // (total - 6) - 1

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
  pkt.insert(pkt.end(), extra.begin(), extra.end());
  return pkt;
}

struct SimSession {
  tcp::socket sock;
  std::array<uint8_t, 2048> buf{};
  exogs::daemon::CcsdsFramer framer;

  bool hk_enabled = false;
  int hk_period_sec = 2;

  uint16_t tm_seq = 100;

  bool verbose = false;

  boost::asio::steady_timer hk_timer;

  explicit SimSession(boost::asio::io_context& io)
    : sock(io), hk_timer(io) {}
};

static void send_bytes(std::shared_ptr<SimSession> s, std::vector<uint8_t>&& data) {
  auto sp = std::make_shared<std::vector<uint8_t>>(std::move(data));
  boost::asio::async_write(s->sock, boost::asio::buffer(*sp),
    [sp](const boost::system::error_code&, std::size_t) {});
}

static void arm_hk(std::shared_ptr<SimSession> s);

static void handle_packet(std::shared_ptr<SimSession> s, std::vector<uint8_t>&& pkt) {
  if (pkt.size() < 8) return;

  const uint16_t pktid = (uint16_t(pkt[0]) << 8) | uint16_t(pkt[1]);
  const uint16_t typ = (pktid >> 12) & 0x1;
  const uint16_t apid = pktid & 0x07FF;
  const uint16_t seqctl = (uint16_t(pkt[2]) << 8) | uint16_t(pkt[3]);
  const uint16_t seq = seqctl & 0x3FFF;

  const uint8_t svc = pkt[6];
  const uint8_t ssvc = pkt[7];

  // Only react to TC packets
  if (typ != 1) return;

  if (s->verbose) {
    print_pkt("[SIM RX]", "TC", apid, seq, svc, ssvc);
  }

  // svc=2 ssvc=1 -> PING
  if (svc == 2 && ssvc == 1) {
    auto tm = build_ccsds(false, apid, s->tm_seq++, 2, 2);
    if (s->verbose) {
      print_pkt("[SIM TX]", "PING", apid, uint16_t(s->tm_seq - 1), 2, 2);
    }
    send_bytes(s, std::move(tm));
    return;
  }

  // svc=3 ssvc=10 -> HK_REQ => reply HK_REPORT
  if (svc == 3 && ssvc == 10) {
    auto tm = build_ccsds(false, apid, s->tm_seq++, 3, 25);
    if (s->verbose) {
      print_pkt("[SIM TX]", "HK_REPORT", apid, uint16_t(s->tm_seq - 1), 3, 25);
    }
    send_bytes(s, std::move(tm));
    return;
  }


  // svc=1 ssvc=1 -> CONNECT
  if (svc == 1 && ssvc == 1) {
    auto tm = build_ccsds(false, apid, s->tm_seq++, 1, 2);
    if (s->verbose) {
      print_pkt("[SIM TX]", "CONNECT", apid, uint16_t(s->tm_seq - 1), 1, 2);
    }
    send_bytes(s, std::move(tm));
    return;
  }

  // svc=1 ssvc=3 -> DISCONNECT
  if (svc == 1 && ssvc == 3) {
    s->hk_enabled = false;
    auto tm = build_ccsds(false, apid, s->tm_seq++, 1, 4);
    if (s->verbose) {
      print_pkt("[SIM TX]", "DISCONNECT", apid, uint16_t(s->tm_seq - 1), 1, 4);
    }
    send_bytes(s, std::move(tm));
    return;
  }

  // svc=3 ssvc=1 -> HK_ENABLE
  if (svc == 3 && ssvc == 1) {
    auto tm = build_ccsds(false, apid, s->tm_seq++, 3, 2);
    if (s->verbose) {
      print_pkt("[SIM TX]", "HK_ENABLE", apid, uint16_t(s->tm_seq - 1), 3, 2);
    }
    send_bytes(s, std::move(tm));
    return;
  }

  // svc=3 ssvc=0 -> HK_DISABLE
  if (svc == 3 && ssvc == 0) {
    s->hk_enabled = false;
    auto tm = build_ccsds(false, apid, s->tm_seq++, 3, 3);
    if (s->verbose) {
      print_pkt("[SIM TX]", "HK_DISABLE", apid, uint16_t(s->tm_seq - 1), 3, 3);
    }
    send_bytes(s, std::move(tm));
    return;
  }

  // Default ACK
  auto tm = build_ccsds(false, apid, s->tm_seq++, svc, 0xFE);
  if (s->verbose) {
    print_pkt("[SIM TX]", "ACK", apid, uint16_t(s->tm_seq - 1), svc, 0xFE);
  }
  send_bytes(s, std::move(tm));
}

static void arm_hk(std::shared_ptr<SimSession> s) {
  s->hk_timer.expires_after(std::chrono::seconds(s->hk_period_sec));
  s->hk_timer.async_wait([s](const boost::system::error_code& ec) {
    if (ec) return;
    if (!s->hk_enabled) return;

    const uint16_t apid = 1;
    const uint16_t seq = s->tm_seq++;
    auto tm = build_ccsds(false, apid, seq, 3, 25);

    if (s->verbose) {
      print_pkt("[SIM TX]", "TM", apid, seq, 3, 25);
    }

    send_bytes(s, std::move(tm));
    arm_hk(s);
  });
}

static void do_read(std::shared_ptr<SimSession> s) {
  s->sock.async_read_some(boost::asio::buffer(s->buf),
    [s](const boost::system::error_code& ec, std::size_t n) {
      if (ec) return;
      s->framer.push_bytes(s->buf.data(), n);
      do_read(s);
    });
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 9000;
  int hk_period = 2;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto need = [&](const char* opt) {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << opt << "\n";
        print_usage(argv[0]);
        std::exit(2);
      }
      return std::string(argv[++i]);
    };

    if (a == "--listen") {
      auto v = need("--listen");
      auto pos = v.find(':');
      if (pos == std::string::npos) {
        std::cerr << "Invalid --listen format\n";
        return 2;
      }
      host = v.substr(0, pos);
      port = static_cast<uint16_t>(std::stoi(v.substr(pos + 1)));
    } else if (a == "--hk-period") {
      hk_period = std::stoi(need("--hk-period"));
    } else if (a == "-v" || a == "--verbose") {
      verbose = true;
    } else if (a == "-h" || a == "--help") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown arg: " << a << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  boost::asio::io_context io;
  tcp::acceptor acc(io, tcp::endpoint(boost::asio::ip::make_address(host), port));

  std::cout << "stm32_sim listening on " << host << ":" << port << "\n";

  std::function<void()> do_accept;
  do_accept = [&]() {
    auto s = std::make_shared<SimSession>(io);
    s->hk_period_sec = hk_period;
    s->verbose = verbose;

    acc.async_accept(s->sock, [&, s](const boost::system::error_code& ec) {
      if (ec) {
        do_accept();
        return;
      }

      std::cout << "stm32_sim: client connected\n";

      s->framer.set_on_packet([s](std::vector<uint8_t>&& pkt) {
        handle_packet(s, std::move(pkt));
      });

      do_read(s);
      do_accept();
    });
  };

  do_accept();
  io.run();
  return 0;
}
