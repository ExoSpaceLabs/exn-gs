#include <boost/asio.hpp>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "exn/shared/protocol.hpp"

using boost::asio::ip::tcp;

void print_usage(const char* exe) {
  std::cerr << "Usage: " << exe << " [--host HOST] [--port PORT] <command> [args...]\n"
            << "Commands:\n"
            << "  ping              Check daemon IPC liveness (PONG)\n"
            << "  status            Query current device-link state\n"
            << "  stats             Query router transport counters\n"
            << "  connect           Open the configured device transport\n"
            << "  disconnect        Close the configured device transport\n"
            << "  reconnect         Cycle the configured device transport\n"
            << "  raw <hex_bytes>   Route one complete CCSDS Space Packet\n";
}

static bool decode_hex(const std::string& hex, std::vector<uint8_t>& bytes) {
  bytes.clear();
  if (hex.empty() || (hex.size() % 2U) != 0U) return false;
  for (const char c : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }

  bytes.reserve(hex.size() / 2U);
  for (size_t i = 0; i < hex.size(); i += 2U) {
    const auto byte_string = hex.substr(i, 2U);
    bytes.push_back(static_cast<uint8_t>(std::strtoul(byte_string.c_str(), nullptr, 16)));
  }
  return true;
}

static bool read_frame(tcp::socket& socket,
                       std::vector<uint8_t>& buffer,
                       exn::proto::Frame& frame) {
  std::array<uint8_t, 4096> tmp{};
  while (!exn::proto::try_decode(buffer, frame)) {
    const size_t n = socket.read_some(boost::asio::buffer(tmp));
    if (n == 0U) return false;
    buffer.insert(buffer.end(), tmp.data(), tmp.data() + n);
  }
  return true;
}

static bool unpack_text(const exn::proto::Frame& frame, std::string& value) {
  return exn::proto::unpack_string(frame.payload, value);
}

static void print_frame(const exn::proto::Frame& frame) {
  std::string text;
  switch (frame.type) {
    case exn::proto::MsgType::Hello:
      if (unpack_text(frame, text)) std::cout << "Daemon: " << text << "\n";
      break;
    case exn::proto::MsgType::LinkState:
      if (unpack_text(frame, text)) std::cout << "Device link: " << text << "\n";
      break;
    case exn::proto::MsgType::QueryResult:
      if (unpack_text(frame, text)) std::cout << text << "\n";
      break;
    case exn::proto::MsgType::Error:
      if (unpack_text(frame, text)) std::cerr << "Daemon error: " << text << "\n";
      break;
    case exn::proto::MsgType::PacketTx: {
      exn::proto::PacketMeta meta;
      std::string desc;
      if (exn::proto::unpack_packet_meta(frame.payload, meta, desc)) {
        std::cout << "Packet TX: APID=" << meta.apid
                  << " SEQ=" << meta.seq
                  << " SVC=" << static_cast<unsigned>(meta.svc)
                  << " SSV=" << static_cast<unsigned>(meta.ssvc)
                  << "\n";
      }
      break;
    }
    default:
      break;
  }
}

int main(int argc, char** argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 7777;
  std::string command;
  std::vector<std::string> args;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = static_cast<uint16_t>(std::stoi(argv[++i]));
    } else if (command.empty()) {
      command = arg;
    } else {
      args.push_back(arg);
    }
  }

  if (command.empty()) {
    print_usage(argv[0]);
    return 1;
  }

  try {
    boost::asio::io_context io;
    tcp::socket socket(io);
    tcp::resolver resolver(io);
    boost::asio::connect(socket, resolver.resolve(host, std::to_string(port)));

    std::vector<uint8_t> rx_buffer;
    rx_buffer.reserve(4096);

    // A daemon session starts with Hello and the current device-link state.
    exn::proto::Frame frame;
    if (!read_frame(socket, rx_buffer, frame)) {
      std::cerr << "Error: daemon closed IPC during handshake\n";
      return 1;
    }
    print_frame(frame);

    exn::proto::Frame request;
    exn::proto::MsgType expected = exn::proto::MsgType::QueryResult;
    bool wait_for_connected_after_reconnect = false;

    if (command == "ping") {
      request = {exn::proto::MsgType::Command, exn::proto::pack_string("PING")};
      expected = exn::proto::MsgType::QueryResult;
    } else if (command == "status") {
      request = {exn::proto::MsgType::Command, exn::proto::pack_string("STATUS")};
      expected = exn::proto::MsgType::LinkState;
    } else if (command == "stats") {
      request = {exn::proto::MsgType::Command, exn::proto::pack_string("STATS")};
      expected = exn::proto::MsgType::QueryResult;
    } else if (command == "connect") {
      request = {exn::proto::MsgType::Command, exn::proto::pack_string("CONNECT")};
      expected = exn::proto::MsgType::LinkState;
    } else if (command == "disconnect") {
      request = {exn::proto::MsgType::Command, exn::proto::pack_string("DISCONNECT")};
      expected = exn::proto::MsgType::LinkState;
    } else if (command == "reconnect") {
      request = {exn::proto::MsgType::Command, exn::proto::pack_string("RECONNECT")};
      expected = exn::proto::MsgType::LinkState;
      wait_for_connected_after_reconnect = true;
    } else if (command == "raw") {
      if (args.empty()) {
        std::cerr << "Error: raw command requires one hex Space Packet\n";
        return 1;
      }
      request.type = exn::proto::MsgType::PacketSend;
      if (!decode_hex(args[0], request.payload)) {
        std::cerr << "Error: raw packet must contain an even number of hexadecimal digits\n";
        return 1;
      }
      expected = exn::proto::MsgType::PacketTx;
    } else {
      std::cerr << "Unknown command: " << command << "\n";
      return 1;
    }

    const auto encoded = exn::proto::encode(request);
    boost::asio::write(socket, boost::asio::buffer(encoded));

    // Ignore unrelated asynchronous notifications until the requested response arrives.
    for (;;) {
      if (!read_frame(socket, rx_buffer, frame)) {
        std::cerr << "Error: daemon closed IPC before replying\n";
        return 1;
      }

      print_frame(frame);
      if (frame.type == exn::proto::MsgType::Error) return 1;
      if (frame.type != expected) continue;

      if (wait_for_connected_after_reconnect && frame.type == exn::proto::MsgType::LinkState) {
        std::string state;
        if (unpack_text(frame, state) &&
            state.rfind("DISCONNECTED", 0) == 0) {
          continue;
        }
      }
      break;
    }

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
