#include "WeReadNativeProtocol.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
namespace WeReadNativeProtocol {
namespace {
// Public constants from the official eink 2.1.2 APK libencrypt.so (SHA in engineering guide).
constexpr uint8_t kRemap[256] = {
    0x34, 0xca, 0x55, 0x40, 0x1d, 0xb6, 0x93, 0xc6, 0x31, 0x30, 0x29, 0x35, 0x32, 0xa7, 0xb8, 0x11, 0xc2, 0xb5, 0x16,
    0xfa, 0x8b, 0xb1, 0x24, 0xa4, 0x10, 0x90, 0x04, 0xe9, 0x08, 0xf8, 0x3b, 0x8a, 0x9c, 0x8c, 0x44, 0xf9, 0xbc, 0x5c,
    0x69, 0xe2, 0xa1, 0xda, 0xd2, 0xd3, 0x75, 0x89, 0xf7, 0x1e, 0x2d, 0x50, 0x56, 0xd7, 0x72, 0x53, 0xbf, 0x22, 0xfb,
    0x20, 0x0f, 0x01, 0x2e, 0x45, 0x87, 0x6e, 0x66, 0x48, 0xf2, 0xe0, 0xcd, 0xfe, 0x67, 0xa9, 0x43, 0xf4, 0x94, 0x51,
    0xce, 0xa5, 0x4a, 0xee, 0x13, 0x26, 0x8e, 0xcc, 0xaa, 0x33, 0x14, 0x5d, 0x0e, 0x39, 0xbb, 0xcf, 0x91, 0x2b, 0x81,
    0x4d, 0xea, 0x99, 0xec, 0x1a, 0x2c, 0x85, 0xc5, 0xd9, 0x36, 0x74, 0x4b, 0x18, 0xe1, 0xf1, 0x3d, 0x9d, 0x41, 0x9f,
    0xb4, 0x17, 0x0d, 0xd6, 0x4c, 0xbe, 0xdc, 0xaf, 0x97, 0x28, 0x77, 0xf0, 0x62, 0xff, 0x71, 0xc1, 0xc8, 0x27, 0x8f,
    0x6c, 0x68, 0xa8, 0x9b, 0xe6, 0x59, 0x1c, 0x1b, 0x12, 0x09, 0x98, 0x4e, 0x3f, 0x06, 0x37, 0x00, 0xba, 0x1f, 0x0a,
    0x19, 0x2f, 0xc9, 0xd5, 0xd0, 0x57, 0x49, 0x6f, 0xfd, 0x25, 0xe4, 0x61, 0x0c, 0x42, 0xcb, 0x96, 0x64, 0x5f, 0xdb,
    0xad, 0x60, 0x23, 0x8d, 0x9a, 0x6d, 0xc3, 0xc4, 0x5e, 0x3e, 0xb9, 0x92, 0x6a, 0xbd, 0x5b, 0x07, 0x7f, 0x76, 0x95,
    0xed, 0x4f, 0xab, 0x84, 0x7a, 0x80, 0xe7, 0x78, 0xc7, 0xe5, 0xeb, 0x73, 0x83, 0x6b, 0xfc, 0x38, 0x46, 0x7d, 0x47,
    0x65, 0xb3, 0x52, 0x63, 0x3a, 0x05, 0xd1, 0xef, 0xa3, 0xa6, 0xde, 0x9e, 0x3c, 0x02, 0xae, 0xb2, 0x7b, 0xa0, 0xf6,
    0xf3, 0x2a, 0xc0, 0xac, 0x86, 0x03, 0x5a, 0x54, 0x0b, 0xf5, 0x82, 0xd4, 0x7e, 0xe3, 0xdf, 0xb0, 0xd8, 0xdd, 0x21,
    0xe8, 0x7c, 0x88, 0xa2, 0x79, 0x58, 0x70, 0xb7, 0x15};
constexpr uint8_t kSalt[] = {'5', 'a', '6', 'f', '1'};
struct Writer {
  char* out;
  size_t cap;
  size_t used = 0;
  bool ok = true;
  void append(const char* format, ...) {
    if (!ok) return;
    va_list args;
    va_start(args, format);
    int n = vsnprintf(out + used, cap - used, format, args);
    va_end(args);
    if (n < 0 || size_t(n) >= cap - used) {
      ok = false;
      return;
    }
    used += n;
  }
};
}  // namespace
bool token(const char* s, bool numeric) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; ++p) {
    if (*p >= '0' && *p <= '9') continue;
    if (!numeric && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_' || *p == '.' ||
                     *p == '+' || *p == '/' || *p == '='))
      continue;
    return false;
  }
  return true;
}
bool sign(const char* const parts[4], Scratch& s, Sha256 hash, char out[65]) {
  struct Slice {
    size_t start, size;
  } slices[5];
  size_t used = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (!parts[i]) return false;
    size_t n = strlen(parts[i]);
    if (used + n + sizeof(kSalt) > sizeof(s.mapped)) return false;
    slices[i] = {used, n};
    for (size_t j = 0; j < n; ++j) {
      uint8_t c = static_cast<uint8_t>(parts[i][j]);
      if (c > 127) return false;
      s.mapped[used++] = kRemap[c];
    }
  }
  slices[4] = {used, sizeof(kSalt)};
  memcpy(s.mapped + used, kSalt, sizeof(kSalt));
  used += sizeof(kSalt);
  std::sort(slices, slices + 5, [&](const Slice& a, const Slice& b) {
    int c = memcmp(s.mapped + a.start, s.mapped + b.start, std::min(a.size, b.size));
    return c < 0 || (c == 0 && a.size < b.size);
  });
  // The caller no longer needs the unsigned payload. Reuse it as binary sorting scratch.
  if (used > sizeof(s.payload)) return false;
  size_t offset = 0;
  for (const auto& part : slices) {
    memcpy(s.payload + offset, s.mapped + part.start, part.size);
    offset += part.size;
  }
  memcpy(s.mapped, s.payload, used);
  for (unsigned round = 0; round < 2; ++round) {
    uint8_t x = 0;
    for (size_t i = 0; i < used; ++i) x ^= s.mapped[i];
    size_t shift = (x % 11) % used;
    if (shift) std::rotate(s.mapped, s.mapped + used - shift, s.mapped + used);
    if (!hash(s.mapped, used, out)) return false;
    used = 64;
    memcpy(s.mapped, out, used);
  }
  return true;
}
bool batch(const Credentials& c, const char* bookId, uint32_t version, const Position& p,
           const WeReadTimeLedger::Hour* hours, unsigned count, const char* guest, const char* config, uint32_t now,
           uint32_t r, Scratch& s, Sha256 hash, char* out, size_t capacity) {
  if (!out || !capacity || !token(c.vid, true) || !token(bookId, true) || !token(c.deviceId) || !token(c.installId) ||
      !token(guest) || !token(config) || !version || now < 1609459200 || r >= 1000 || !count || count > 16)
    return false;
  uint32_t total = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (hours[i].start < 1609459200 || hours[i].start % 3600 || hours[i].start > now || !hours[i].amount ||
        hours[i].amount > 3600 || hours[i].amount > now - hours[i].start)
      return false;
    for (unsigned j = 0; j < i; ++j)
      if (hours[i].start == hours[j].start) return false;
    total += hours[i].amount;
  }
  Writer payload{s.payload, sizeof(s.payload)};
  payload.append("%s_%s_%s_%s_0_+08:00_%u_0", c.vid, c.deviceId, c.deviceId, bookId, unsigned(total));
  for (unsigned i = 0; i < count; ++i)
    payload.append("_%u_+08:00_%u_0", unsigned(hours[i].start), unsigned(hours[i].amount));
  char randomText[16], timeText[24], inner[65], outer[65];
  snprintf(randomText, sizeof(randomText), "%u", unsigned(r));
  snprintf(timeText, sizeof(timeText), "%u", unsigned(now));
  const char* parts[] = {guest, randomText, timeText, s.payload};
  if (!payload.ok || !sign(parts, s, hash, inner)) return false;
  int n = snprintf(s.payload, sizeof(s.payload), "%llu%s%u", static_cast<unsigned long long>(now) * 1000, config,
                   unsigned(r));
  if (n < 0 || size_t(n) >= sizeof(s.payload) || !hash(reinterpret_cast<uint8_t*>(s.payload), n, outer)) return false;
  Writer w{out, capacity};
  w.append(
      "{\"books\":[{\"bookId\":\"%s\",\"bookVersion\":%u,\"appId\":\"%s\",\"deviceId\":\"%s\",\"installId\":\"%s\","
      "\"readingTime\":%u,\"ttsTime\":0,\"lectureTime\":0,\"lectureTextTime\":0,\"novalTime\":0,\"autoTime\":0,"
      "\"isLecture\":0,\"isResendReadingInfo\":1,\"finish\":false,\"risk\":0,\"recordCreateTimeZone\":\"+08:00\","
      "\"hours\":[",
      bookId, unsigned(version), c.deviceId, c.deviceId, c.installId, unsigned(total));
  for (unsigned i = 0; i < count; ++i)
    w.append("%s{\"startOfHour\":%u,\"timeZone\":\"+08:00\",\"readingTime\":%u,\"ttsTime\":0}", i ? "," : "",
             unsigned(hours[i].start), unsigned(hours[i].amount));
  w.append(
      "],\"timestamp\":%u,\"random\":%u,\"summary\":\"\",\"progress\":%u,\"currentProgress\":%u,"
      "\"chapterUid\":%u,\"chapterIdx\":%u,\"chapterOffset\":%u,\"signature\":\"%s\"}],"
      "\"timestamp\":%llu,\"random\":%u,\"signature\":\"%s\",\"recordCreateTimeZone\":\"+08:00\"}",
      unsigned(now), unsigned(r), unsigned(p.progress), unsigned(p.currentProgress), unsigned(p.chapterUid),
      unsigned(p.chapterIdx), unsigned(p.chapterOffset), inner, static_cast<unsigned long long>(now) * 1000,
      unsigned(r), outer);
  return w.ok;
}
}  // namespace WeReadNativeProtocol
