#include "exogs/daemon/app.hpp"
#include "exogs/daemon/ipc_server.hpp"
#include "exogs/daemon/serial_link.hpp"
#include "exogs/daemon/state.hpp"
#include "exogs/daemon/storage.hpp"
#include "exogs/shared/daemon/framer.hpp"
#include "exogs/shared/protocol.hpp"
#include "exogs/shared/time.hpp"
#include "exogs/shared/types.hpp"

#include <iomanip>
#include <boost/asio.hpp>
#include <filesystem>
#include <iostream>
#include <memory>

namespace exogs::daemon {

static void print_packet(const char* tag,
                       const exogs::proto::PacketMeta& m,
                       const std::string& desc) {
  std::cout
    << tag
    << " APID=" << m.apid
    << " SEQ=" << m.seq
    << " SVC=" << int(m.svc)
    << " SSV=" << int(m.ssvc)
    << " LEN=" << m.len
    << " " << desc
    << "\n";
}


static std::string link_state_to_string(exogs::LinkState s) {
  switch (s) {
    case exogs::LinkState::Disconnected: return "DISCONNECTED";
    case exogs::LinkState::Connected: return "CONNECTED";
    case exogs::LinkState::Error: return "ERROR";
  }
  return "UNKNOWN";
}

App::App(AppConfig cfg) : cfg_(std::move(cfg)) {}

static exogs::proto::PacketMeta decode_primary_header_meta(const std::vector<uint8_t>& pkt, exogs::Direction dir_hint) {
  exogs::proto::PacketMeta m;
  m.ts_ns = exogs::now_ns();
  if (pkt.size() < 6) return m;

  const uint16_t pktid = (uint16_t(pkt[0]) << 8) | uint16_t(pkt[1]);
  const uint16_t seq = (uint16_t(pkt[2]) << 8) | uint16_t(pkt[3]);
  const uint16_t len = (uint16_t(pkt[4]) << 8) | uint16_t(pkt[5]);

  m.ver = uint8_t((pktid >> 13) & 0x07);
  m.typ = uint8_t((pktid >> 12) & 0x01);

  if (dir_hint == exogs::Direction::TC) m.typ = 1;
  if (dir_hint == exogs::Direction::TM) m.typ = 0;

  m.shf = uint8_t((pktid >> 11) & 0x01);
  m.apid = uint16_t(pktid & 0x07FFu);
  m.seqf = uint8_t((seq >> 14) & 0x03);
  m.seq = uint16_t(seq & 0x3FFFu);
  m.len = len;
  return m;
}

static exogs::proto::Frame make_packet_frame(exogs::proto::MsgType t, const exogs::proto::PacketMeta& m, const std::string& desc) {
  return exogs::proto::Frame{t, exogs::proto::pack_packet_meta(m, desc)};
}

static std::vector<uint8_t> build_ccsds_tc(uint16_t apid, uint16_t seq, uint8_t svc, uint8_t ssvc,
                                           const std::vector<uint8_t>& extra = {}) {
  const uint16_t ver = 0;
  const uint16_t typ = 1;
  const uint16_t shf = 1;

  const uint16_t pktid = uint16_t((ver & 0x7) << 13) | uint16_t((typ & 0x1) << 12) |
                         uint16_t((shf & 0x1) << 11) | uint16_t(apid & 0x07FF);

  const uint16_t seqf = 3; // standalone
  const uint16_t seqctl = uint16_t((seqf & 0x3) << 14) | uint16_t(seq & 0x3FFF);

  const size_t payload_sz = 2 + extra.size();
  const uint16_t ccsds_len = uint16_t(payload_sz - 1);

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

int App::run() {
  std::filesystem::create_directories(cfg_.log_dir);
  boost::asio::io_context io;
  boost::asio::steady_timer hk_tc_timer(io);
  bool hk_tc_enabled = false;  // enable after UI CONNECT

  StateStore state(2000);
  auto logger = make_file_logger_sink(cfg_.log_dir);
  IpcServer ipc(io, cfg_.listen_host, cfg_.listen_port);

  SerialLink link(io, cfg_.serial_port, cfg_.baud);
  CcsdsFramer framer;
  static uint16_t tc_seq = 1;

  std::function<void()> arm_hk_tc;
  arm_hk_tc = [&]() {
    hk_tc_timer.expires_after(std::chrono::seconds(2));
    hk_tc_timer.async_wait([&](const boost::system::error_code& ec) {
      if (ec) return;

      if (hk_tc_enabled && link.opened()) {
        // Periodic HK request TC (svc=3, ssvc=10 for example)
        auto pkt = build_ccsds_tc(/*apid*/1, tc_seq++, /*svc*/3, /*ssvc*/10);
        link.write_bytes(pkt.data(), pkt.size());

        auto m = decode_primary_header_meta(pkt, exogs::Direction::TC);
        m.svc = 3; m.ssvc = 10;
        ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketTx, m, "HK_REQ"));
        if (cfg_.verbose) print_packet("[TC->LINK]", m, "HK_REQ");
      }

      arm_hk_tc();
    });
  };
  arm_hk_tc();


  auto publish_link_state = [&]() {
    const auto snap = state.snapshot();
    std::string payload = link_state_to_string(snap.link);
    if (!snap.link_detail.empty()) payload += " " + snap.link_detail;
    ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::LinkState, exogs::proto::pack_string(payload)});
  };

  link.set_on_state([&](bool opened, const std::string& detail) {
    state.set_link(opened ? exogs::LinkState::Connected : exogs::LinkState::Disconnected, detail);
    publish_link_state();
  });

  link.set_on_error([&](const std::string& err) {
    state.set_link(exogs::LinkState::Error, err);
    publish_link_state();
  });

  framer.set_on_packet([&](std::vector<uint8_t>&& pkt) {
    exogs::PacketRecord rec;
    rec.ts_ns = exogs::now_ns();
    rec.dir = exogs::Direction::TM;

    if (pkt.size() >= 8) {
      const uint16_t pktid = (uint16_t(pkt[0]) << 8) | uint16_t(pkt[1]);
      const uint16_t seqc = (uint16_t(pkt[2]) << 8) | uint16_t(pkt[3]);
      rec.apid = pktid & 0x07FFu;
      rec.seq = seqc & 0x3FFFu;
      rec.service = pkt[6];
      rec.subservice = pkt[7];
    }

    rec.summary = "RX";
    rec.raw = std::move(pkt);

    state.on_packet(rec);
    logger->store(rec);

    auto m = decode_primary_header_meta(rec.raw, exogs::Direction::TM);
    m.svc = rec.service;
    m.ssvc = rec.subservice;
    ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketRx, m, "TM"));
    if (cfg_.verbose) {
      print_packet("[TM<-LINK]", m, "TM");
    }
  });

  link.set_on_bytes([&](const uint8_t* data, size_t n) {
    framer.push_bytes(data, n);
  });

  ipc.set_on_command([&](const exogs::proto::Frame& f, std::shared_ptr<IpcSession>) {
    std::string cmd;
    if (!exogs::proto::unpack_string(f.payload, cmd)) return;

    if (cmd == "CONNECT") {
      link.start();

      // Send CONNECT TC
      auto pkt = build_ccsds_tc(/*apid*/1, tc_seq++, /*svc*/1, /*ssvc*/1);
      link.write_bytes(pkt.data(), pkt.size());

      auto m = decode_primary_header_meta(pkt, exogs::Direction::TC);
      m.svc = 1; m.ssvc = 1;
      ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketTx, m, "CONNECT"));
      if (cfg_.verbose) {
        print_packet("[TC->LINK]", m, "CONNECT");
      }
      // Send HK_ENABLE TC (sim starts periodic HK_REPORT)
      std::vector<uint8_t> extra; // optional period later
      auto hk = build_ccsds_tc(/*apid*/1, tc_seq++, /*svc*/3, /*ssvc*/1, extra);
      link.write_bytes(hk.data(), hk.size());

      auto mhk = decode_primary_header_meta(hk, exogs::Direction::TC);
      mhk.svc = 3; mhk.ssvc = 1;
      ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketTx, mhk, "HK_ENABLE"));
      if (cfg_.verbose) {
        print_packet("[TC->LINK]", m, "HK_ENABLE");
      }
      hk_tc_enabled = true;
      return;
    }

    if (cmd == "DISCONNECT") {
      // Send DISCONNECT TC before closing
      auto pkt = build_ccsds_tc(/*apid*/1, tc_seq++, /*svc*/1, /*ssvc*/3);
      link.write_bytes(pkt.data(), pkt.size());

      auto m = decode_primary_header_meta(pkt, exogs::Direction::TC);
      m.svc = 1; m.ssvc = 3;
      ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketTx, m, "DISCONNECT"));
      if (cfg_.verbose) {
        print_packet("[TC->LINK]", m, "DISCONNECT");
      }
      hk_tc_enabled = false;
      link.stop();
      return;
    }

    if (cmd == "PING") {
      auto pkt = build_ccsds_tc(/*apid*/1, tc_seq++, /*svc*/2, /*ssvc*/1);
      link.write_bytes(pkt.data(), pkt.size());

      auto m = decode_primary_header_meta(pkt, exogs::Direction::TC);
      m.svc = 2; m.ssvc = 1;
      ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketTx, m, "PING"));
      if (cfg_.verbose) {
        print_packet("[TC->LINK]", m, "PING");
      }
      return;
    }

    // For future: "SEND ..." maps to a TC builder
  });

  ipc.start();

  state.set_link(exogs::LinkState::Disconnected, "");
  publish_link_state();

  std::cout << "exo_gsd listening on " << cfg_.listen_host << ":" << cfg_.listen_port << "\n";
  std::cout << "Link port: " << (cfg_.serial_port.empty() ? "(none)" : cfg_.serial_port) << "\n";
  // Auto-connect to the device immediately (serial or tcp://...), but don't send anything yet.
  if (!cfg_.serial_port.empty()) {
    link.start();
  }

  io.run();
  return 0;
}

} // namespace exogs::daemon
