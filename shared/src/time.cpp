#include "exn/shared/time.hpp"
#include <chrono>

namespace exn {

  uint64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
  }

} // namespace exn
