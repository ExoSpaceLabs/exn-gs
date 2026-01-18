#include "exogs/shared/time.hpp"
#include <chrono>

namespace exogs {
uint64_t now_ns() {
  using namespace std::chrono;
  return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace exogs
