#include "exn/daemon/storage.hpp"
#include <filesystem>
#include <fstream>
#include <mutex>

namespace exn::daemon {

class FileLoggerSink final : public IStorageSink {
public:
  explicit FileLoggerSink(std::string dir) : dir_(std::move(dir)) {
    std::filesystem::create_directories(dir_);
    out_.open(std::filesystem::path(dir_) / "packets.jsonl", std::ios::app);
  }

  void store(const exn::PacketRecord& rec) override {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!out_) return;
    // Minimal JSONL without extra deps.
    out_ << "{"
         << "\"ts_ns\":" << rec.ts_ns << ","
         << "\"dir\":" << (rec.dir == exn::Direction::TC ? "\"TC\"" : "\"TM\"") << ","
         << "\"apid\":" << rec.apid << ","
         << "\"seq\":" << rec.seq << ","
         << "\"service\":" << unsigned(rec.service) << ","
         << "\"subservice\":" << unsigned(rec.subservice) << ","
         << "\"summary\":\"" << escape(rec.summary) << "\""
         << "}\n";
    out_.flush();
  }

private:
  static std::string escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
      switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default: o += c; break;
      }
    }
    return o;
  }

  std::string dir_;
  std::ofstream out_;
  std::mutex mtx_;
};

std::unique_ptr<IStorageSink> make_file_logger_sink(const std::string& dir) {
  return std::make_unique<FileLoggerSink>(dir);
}

} // namespace exn::daemon
