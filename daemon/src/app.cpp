#include "exogs/daemon/app.hpp"
#include "exogs/daemon/framer.hpp"
#include "exogs/daemon/ipc_server.hpp"
#include "exogs/daemon/serial_link.hpp"
#include "exogs/daemon/state.hpp"
#include "exogs/daemon/storage.hpp"
#include "exogs/shared/protocol.hpp"
#include "exogs/shared/time.hpp"
#include "exogs/shared/types.hpp"

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <filesystem>
#include <iostream>
#include <memory>

namespace exogs::daemon {

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

  // If a caller provided a direction hint, override typ accordingly.
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

int App::run() {
  std::filesystem::create_directories(cfg_.log_dir);

  boost::asio::io_context io;

  StateStore state(2000);
  auto logger = make_file_logger_sink(cfg_.log_dir);

  IpcServer ipc(io, cfg_.listen_host, cfg_.listen_port);

  boost::asio::steady_timer demo_timer(io);
  bool demo_stream_enabled = false;
  static uint16_t demo_tc_seq = 0;
  static uint16_t demo_tm_seq = 100;

  SerialLink serial(io, cfg_.serial_port, cfg_.baud);
  CcsdsFramer framer;

  auto demo_send_tc = [&](uint16_t apid, uint8_t svc, uint8_t ssvc, const std::string& desc) {
    exogs::proto::PacketMeta m;
    m.ts_ns = exogs::now_ns();
    m.ver = 0;
    m.typ = 1;
    m.shf = 1;
    m.apid = apid;
    m.seqf = 3;
    m.seq = uint16_t(demo_tc_seq++ & 0x3FFFu);
    m.len = 0; // demo
    m.svc = svc;
    m.ssvc = ssvc;
    ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketTx, m, desc));
    return m;
  };

  auto demo_send_tm = [&](uint16_t apid, uint8_t svc, uint8_t ssvc, const std::string& desc) {
    exogs::proto::PacketMeta m;
    m.ts_ns = exogs::now_ns();
    m.ver = 0;
    m.typ = 0;
    m.shf = 1;
    m.apid = apid;
    m.seqf = 3;
    m.seq = uint16_t(demo_tm_seq++ & 0x3FFFu);
    m.len = 0; // demo
    m.svc = svc;
    m.ssvc = ssvc;
    ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketRx, m, desc));
    return m;
  };

  auto send_link_state = [&]() {
    const auto snap = state.snapshot();
    const std::string payload = link_state_to_string(snap.link) + " " + snap.link_detail;
    ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::LinkState, exogs::proto::pack_string(payload)});
  };

  framer.set_on_packet([&](std::vector<uint8_t>&& pkt) {
    exogs::PacketRecord rec;
    rec.ts_ns = exogs::now_ns();
    rec.dir = exogs::Direction::TM;

    if (pkt.size() >= 6) {
      const uint16_t pktid = (uint16_t(pkt[0]) << 8) | uint16_t(pkt[1]);
      const uint16_t seq = (uint16_t(pkt[2]) << 8) | uint16_t(pkt[3]);
      rec.apid = pktid & 0x07FFu;
      rec.seq = seq & 0x3FFFu;
      rec.service = 3;
      rec.subservice = 25;
    }

    rec.summary = "RX";
    rec.raw = std::move(pkt);

    state.on_packet(rec);
    logger->store(rec);

    auto m = decode_primary_header_meta(rec.raw, exogs::Direction::TM);
    m.svc = rec.service;
    m.ssvc = rec.subservice;
    ipc.broadcast(make_packet_frame(exogs::proto::MsgType::PacketRx, m, "RX CCSDS"));
  });

  serial.set_on_bytes([&](const uint8_t* data, size_t n) {
    framer.push_bytes(data, n);
  });

  serial.set_on_error([&](const std::string& err) {
    state.set_link(exogs::LinkState::Error, err);
    send_link_state();
  });

  ipc.set_on_command([&](const exogs::proto::Frame& f, std::shared_ptr<IpcSession> sess) {
    (void)sess;

    std::string cmd;
    if (!exogs::proto::unpack_string(f.payload, cmd)) return;

    if (cmd == "PING") {
      demo_send_tc(/*apid*/1, /*svc*/2, /*ssvc*/1, "PING");
      demo_send_tm(/*apid*/1, /*svc*/2, /*ssvc*/2, "PING_REPLY");
    } else if (cmd == "CONNECT") {
      serial.start();
      state.set_link(exogs::LinkState::Connected, cfg_.serial_port.empty() ? "(demo)" : cfg_.serial_port);
      send_link_state();

      // “CONNECT” TC + ACK
      demo_send_tc(/*apid*/1, /*svc*/1, /*ssvc*/1, "CONNECT");
      demo_send_tm(/*apid*/1, /*svc*/1, /*ssvc*/2, "ACK_CONNECT");

      // Now explicitly request HK streaming in demo:

      demo_send_tc(/*apid*/1, /*svc*/3, /*ssvc*/1, "HK_ENABLE periodic=2s");
      demo_send_tm(/*apid*/1, /*svc*/3, /*ssvc*/2, "ACK_HK_ENABLE");

      demo_stream_enabled = true;
    } else if (cmd == "DISCONNECT") {
      serial.stop();
      state.set_link(exogs::LinkState::Disconnected, "");
      send_link_state();
      demo_send_tc(/*apid*/1, /*svc*/1, /*ssvc*/3, "DISCONNECT");
      demo_stream_enabled = false;
      demo_send_tm(/*apid*/1, /*svc*/1, /*ssvc*/4, "ACK_DISCONNECT");
    } else if (cmd.rfind("SEND ", 0) == 0) {
      demo_send_tc(/*apid*/1, /*svc*/9, /*ssvc*/1, cmd);
      demo_send_tm(/*apid*/1, /*svc*/9, /*ssvc*/2, "ACK_SEND");
    }
  });

  ipc.start();

  state.set_link(exogs::LinkState::Disconnected, "");
  send_link_state();

  // Demo generator: periodic HK only when CONNECTED in demo mode.
  std::function<void()> arm_demo;
  arm_demo = [&]() {
    demo_timer.expires_after(std::chrono::seconds(2));
    demo_timer.async_wait([&](const boost::system::error_code& ec) {
      if (ec) return;
      if (cfg_.serial_port.empty() && demo_stream_enabled) {
        demo_send_tc(/*apid*/1, /*svc*/3, /*ssvc*/4, "HK_REQ");
        demo_send_tm(/*apid*/1, /*svc*/3, /*ssvc*/25, "HK_REPORT");
      }
      arm_demo();
    });
  };
  arm_demo();

  std::cout << "exo_gsd listening on " << cfg_.listen_host << ":" << cfg_.listen_port << "\n";
  if (!cfg_.serial_port.empty()) {
    std::cout << "Serial configured: " << cfg_.serial_port << " @" << cfg_.baud << "\n";
  } else {
    std::cout << "Serial not configured: running in demo mode\n";
  }

  io.run();
  return 0;
}

} // namespace exogs::daemon
