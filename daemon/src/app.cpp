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

int App::run() {
  std::filesystem::create_directories(cfg_.log_dir);

  boost::asio::io_context io;

  StateStore state(2000);
  auto logger = make_file_logger_sink(cfg_.log_dir);

  IpcServer ipc(io, cfg_.listen_host, cfg_.listen_port);

  // Demo timer: produces fake packets if serial is not configured or idle.
  boost::asio::steady_timer demo_timer(io);

  SerialLink serial(io, cfg_.serial_port, cfg_.baud);
  CcsdsFramer framer;

  auto send_link_state = [&]() {
    const auto snap = state.snapshot();
    const std::string payload = link_state_to_string(snap.link) + " " + snap.link_detail;
    ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::LinkState, exogs::proto::pack_string(payload)});
  };

  framer.set_on_packet([&](std::vector<uint8_t>&& pkt) {
    // TODO: Use CCSDSPack to decode. For now, do a minimal decode:
    // primary header is 6 bytes: [0..1]=pktid, [2..3]=seq, [4..5]=len
    exogs::PacketRecord rec;
    rec.ts_ns = exogs::now_ns();
    // Bytes coming from the STM side are telemetry from GS perspective.
    rec.dir = exogs::Direction::TM;
    if (pkt.size() >= 6) {
      const uint16_t pktid = (uint16_t(pkt[0]) << 8) | uint16_t(pkt[1]);
      const uint16_t seq = (uint16_t(pkt[2]) << 8) | uint16_t(pkt[3]);
      rec.apid = pktid & 0x07FFu;
      rec.seq = seq & 0x3FFFu;
    }
    // fake service/subservice unless you implement secondary header.
    rec.service = 3;
    rec.subservice = 25;
    rec.summary = "RX CCSDS packet";
    rec.raw = std::move(pkt);

    state.on_packet(rec);
    logger->store(rec);

    // For now, event payload is a single summary string.
    // v1: encode full record.
    std::string s = "TM apid=" + std::to_string(rec.apid) + " seq=" + std::to_string(rec.seq) +
                    " srv=" + std::to_string(rec.service) + "/" + std::to_string(rec.subservice) +
                    " " + rec.summary;
    ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::PacketRx, exogs::proto::pack_string(s)});
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
    // v0: only string commands.
    std::string cmd;
    if (!exogs::proto::unpack_string(f.payload, cmd)) return;

    if (cmd == "PING") {
      ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::Hello, exogs::proto::pack_string("PONG")});
      ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::PacketTx, exogs::proto::pack_string("TC cmd=PING")});
    } else if (cmd == "CONNECT") {
      serial.start();
      state.set_link(exogs::LinkState::Connected, cfg_.serial_port.empty() ? "(demo)" : cfg_.serial_port);
      send_link_state();
      ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::PacketTx, exogs::proto::pack_string("TC cmd=CONNECT")});
    } else if (cmd == "DISCONNECT") {
      serial.stop();
      state.set_link(exogs::LinkState::Disconnected, "");
      send_link_state();
      ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::PacketTx, exogs::proto::pack_string("TC cmd=DISCONNECT")});
    } else if (cmd.rfind("SEND ", 0) == 0) {
      // TODO: implement TX path
      ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::Hello, exogs::proto::pack_string("TX not implemented")});
      ipc.broadcast(exogs::proto::Frame{exogs::proto::MsgType::PacketTx, exogs::proto::pack_string("TC " + cmd)});
    }
  });

  ipc.start();

  // Initial link state
  state.set_link(exogs::LinkState::Disconnected, "");
  send_link_state();

  // Demo generator: send a fake packet every 2s in demo mode (no serial port set)
  std::function<void()> arm_demo;
  arm_demo = [&]() {
    demo_timer.expires_after(std::chrono::seconds(2));
    demo_timer.async_wait([&](const boost::system::error_code& ec) {
      if (ec) return;
      if (cfg_.serial_port.empty()) {
        // Create a minimal CCSDS packet so the framer path is exercised.
        std::vector<uint8_t> pkt;
        pkt.resize(6 + 1); // 6 header + 1 data
        // pktid: version/type/sec + apid. Put apid=1
        pkt[0] = 0x08; // version=0, type=0, sec=1 -> 0b00001000
        pkt[1] = 0x01;
        // seq: 0xC000 | seqcount
        static uint16_t seq = 0;
        uint16_t seqf = 0xC000 | (seq++ & 0x3FFF);
        pkt[2] = uint8_t(seqf >> 8);
        pkt[3] = uint8_t(seqf & 0xFF);
        // length = (total - 6) - 1 = (7-6)-1 =0
        pkt[4] = 0;
        pkt[5] = 0;
        pkt[6] = 0xAA;
        framer.push_bytes(pkt.data(), pkt.size());
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
