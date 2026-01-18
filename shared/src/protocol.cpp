#include "exogs/shared/protocol.hpp"

namespace exogs::proto {

static void append_u32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t((x >> 24) & 0xFF));
  v.push_back(uint8_t((x >> 16) & 0xFF));
  v.push_back(uint8_t((x >> 8) & 0xFF));
  v.push_back(uint8_t(x & 0xFF));
}
static void append_u16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t((x >> 8) & 0xFF));
  v.push_back(uint8_t(x & 0xFF));
}
static uint32_t read_u32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
static uint16_t read_u16(const uint8_t* p) {
  return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

std::vector<uint8_t> encode(const Frame& f) {
  std::vector<uint8_t> out;
  const uint32_t len = 2u + static_cast<uint32_t>(f.payload.size());
  out.reserve(4u + len);
  append_u32(out, len);
  append_u16(out, static_cast<uint16_t>(f.type));
  out.insert(out.end(), f.payload.begin(), f.payload.end());
  return out;
}

bool try_decode(std::vector<uint8_t>& buffer, Frame& out) {
  if (buffer.size() < 4) return false;
  const uint32_t len = read_u32(buffer.data());
  if (len < 2) return false;
  if (buffer.size() < 4u + len) return false;

  const uint16_t t = read_u16(buffer.data() + 4);
  out.type = static_cast<MsgType>(t);
  out.payload.assign(buffer.begin() + 6, buffer.begin() + 4 + len);
  buffer.erase(buffer.begin(), buffer.begin() + 4 + len);
  return true;
}

std::vector<uint8_t> pack_string(const std::string& s) {
  std::vector<uint8_t> p;
  p.reserve(4 + s.size());
  append_u32(p, static_cast<uint32_t>(s.size()));
  p.insert(p.end(), s.begin(), s.end());
  return p;
}

bool unpack_string(const std::vector<uint8_t>& p, std::string& s) {
  if (p.size() < 4) return false;
  const uint32_t n = read_u32(p.data());
  if (p.size() < 4u + n) return false;
  s.assign(reinterpret_cast<const char*>(p.data() + 4), reinterpret_cast<const char*>(p.data() + 4 + n));
  return true;
}

} // namespace exogs::proto
