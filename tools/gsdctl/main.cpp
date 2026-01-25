#include <iostream>
#include <string>
#include <vector>
#include <boost/asio.hpp>
#include "exn/shared/protocol.hpp"

using boost::asio::ip::tcp;

void print_usage(const char* exe) {
    std::cerr << "Usage: " << exe << " [--host HOST] [--port PORT] <command> [args...]\n"
              << "Commands:\n"
              << "  ping              Send a ping command to the daemon\n"
              << "  raw <hex_bytes>   Send raw bytes as a Command frame\n";
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

        // Wait for Hello
        std::vector<uint8_t> rx_buf(1024);
        size_t n = socket.read_some(boost::asio::buffer(rx_buf));
        rx_buf.resize(n);
        exn::proto::Frame hello_frame;
        if (exn::proto::try_decode(rx_buf, hello_frame)) {
            if (hello_frame.type == exn::proto::MsgType::Hello) {
                std::string server_name;
                if (exn::proto::unpack_string(hello_frame.payload, server_name)) {
                    std::cout << "Connected to " << server_name << "\n";
                }
            }
        }

        exn::proto::Frame f;
        if (command == "ping") {
            f.type = exn::proto::MsgType::Command;
            f.payload = exn::proto::pack_string("ping");
        } else if (command == "raw") {
            if (args.empty()) {
                std::cerr << "Error: raw command requires hex bytes\n";
                return 1;
            }
            f.type = exn::proto::MsgType::Command;
            // Simple hex to bytes (expects no spaces, e.g. 010203)
            std::string hex = args[0];
            for (size_t i = 0; i < hex.length(); i += 2) {
                std::string byteString = hex.substr(i, 2);
                uint8_t byte = (uint8_t) strtol(byteString.c_str(), NULL, 16);
                f.payload.push_back(byte);
            }
        } else {
            std::cerr << "Unknown command: " << command << "\n";
            return 1;
        }

        auto encoded = exn::proto::encode(f);
        boost::asio::write(socket, boost::asio::buffer(encoded));
        std::cout << "Command sent.\n";

    } catch (std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
