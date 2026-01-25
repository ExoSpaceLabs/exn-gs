#pragma once
#include "exn/shared/types.hpp"
#include <memory>

namespace exn::daemon {

class IStorageSink {
public:
  virtual ~IStorageSink() = default;
  virtual void store(const exn::PacketRecord& rec) = 0;
};

std::unique_ptr<IStorageSink> make_file_logger_sink(const std::string& dir);

} // namespace exn::daemon
