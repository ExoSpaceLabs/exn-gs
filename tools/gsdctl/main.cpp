#include <boost/asio.hpp>
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
            << "  ping              Query daemon/link state without sending a spacecraft TC\n"
            << "  raw <hex_bytes>   Route one complete raw CCSDS TC Space Packet\n";
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

    // The first daemon frame is Hello. A blocking tool can read it in one framed transaction.
    std::vector<uint8_t> rx_buf(4096);
    size_t n = socket.read_some(boost::asio::buffer(rx_buf));
    rx_buf.resize(n);
    exn::proto::Frame hello_frame;
    if (exn::proto::try_decode(rx_buf, hello_frame) &&
        hello_frame.type == exn::proto::MsgType::Hello) {
      std::string server_name;
      if (exn::proto::unpack_string(hello_frame.payload, server_name)) {
        std::cout << "Connected to " << server_name << "\n";
      }
    }

    exn::proto::Frame frame;
    if (command == "ping") {
      frame.type = exn::proto::MsgType::Command;
      frame.payload = exn::proto::pack_string("PING");
    } else if (command == "raw") {
      if (args.empty()) {
        std::cerr << "Error: raw command requires one hex Space Packet\n";
        return 1;
      }
      frame.type = exn::proto::MsgType::PacketSend;
      if (!decode_hex(args[0], frame.payload)) {
        std::cerr << "Error: raw packet must contain an even number of hexadecimal digits\n";
        return 1;
      }
    } else {
      std::cerr << "Unknown command: " << command << "\n";
      return 1;
    }

    const auto encoded = exn::proto::encode(frame);
    boost::asio::write(socket, boost::asio::buffer(encoded));
    std::cout << "Request sent.\n";

  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
