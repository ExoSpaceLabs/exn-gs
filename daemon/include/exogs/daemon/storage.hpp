#pragma once
#include "exogs/shared/types.hpp"
#include <memory>

namespace exogs::daemon {

class IStorageSink {
public:
  virtual ~IStorageSink() = default;
  virtual void store(const exogs::PacketRecord& rec) = 0;
};

std::unique_ptr<IStorageSink> make_file_logger_sink(const std::string& dir);

} // namespace exogs::daemon
