#include "WeReadTimeLedger.h"

#include <HalStorage.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
constexpr const char* kRoot = "/.crosspoint/weread-time-v2";
constexpr size_t kRecordSize = 40 + 8 * (WeReadTimeLedger::kCapacity + WeReadTimeLedger::kBatchHours) + 4;
constexpr uint32_t kMinEpoch = 1609459200;
bool safeId(const char* value) {
  if (!value || !value[0]) return false;
  size_t n = 0;
  for (; value[n] && n < 64; ++n) {
    const char c = value[n];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')) return false;
  }
  return n < 64;
}
void put(uint8_t* p, uint64_t v, unsigned n) {
  for (unsigned i = 0; i < n; ++i) {
    p[i] = v & 255;
    v >>= 8;
  }
}
uint64_t get(const uint8_t* p, unsigned n) {
  uint64_t v = 0;
  for (unsigned i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
  return v;
}
uint32_t checksum(const uint8_t* p) {
  uint32_t v = 2166136261U;
  for (size_t i = 0; i < kRecordSize - 4; ++i) v = (v ^ p[i]) * 16777619U;
  return v;
}
bool validHour(uint32_t h) { return h >= kMinEpoch && h % 3600 == 0; }
}  // namespace

bool WeReadTimeLedger::readSlot(unsigned slot, Record& r, bool& exists) const {
  char name[184];
  snprintf(name, sizeof(name), "%s.%u", path_, slot);
  exists = Storage.exists(name);
  if (!exists) return false;
  uint8_t b[kRecordSize];
  HalFile f;
  if (!Storage.openFileForRead("WRTime", name, f) || f.fileSize64() != sizeof(b) ||
      f.read(b, sizeof(b)) != int(sizeof(b)) || memcmp(b, "WRTM", 4) || b[4] != 2 || b[5] || b[6] || b[7] ||
      get(b + sizeof(b) - 4, 4) != checksum(b))
    return false;
  r = {};
  r.sequence = get(b + 8, 8);
  r.acceptedSeconds = get(b + 16, 8);
  r.uncertainSeconds = get(b + 24, 8);
  r.unknownMs = get(b + 32, 8);
  size_t off = 40;
  for (unsigned i = 0; i < kCapacity + kBatchHours; ++i, off += 8) {
    Hour& h = i < kCapacity ? r.pending[i] : r.flight[i - kCapacity];
    h.start = get(b + off, 4);
    h.amount = get(b + off + 4, 4);
    if ((!h.start && h.amount) || (h.start && !validHour(h.start)) || h.amount > (i < kCapacity ? 3600000U : 3600U))
      return false;
    if (i < kCapacity && h.start)
      for (unsigned j = 0; j < i; ++j)
        if (r.pending[j].start == h.start) return false;
  }
  return r.sequence > 0;
}
bool WeReadTimeLedger::open(const char* account, const char* book) {
  healthy_ = false;
  dirty_ = false;
  record_ = {};
  if (!safeId(account) || !safeId(book) || strncmp(book, "MP_WXS_", 7) == 0) return false;
  char directory[96];
  snprintf(directory, sizeof(directory), "%s/%s", kRoot, account);
  if (!Storage.ensureDirectoryExists(kRoot) || !Storage.ensureDirectoryExists(directory)) return false;
  snprintf(path_, sizeof(path_), "%s/%s", directory, book);
  snprintf(account_, sizeof(account_), "%s", account);
  Record other;
  bool e0 = false, e1 = false;
  bool v0 = readSlot(0, record_, e0), v1 = readSlot(1, other, e1);
  if (!v0 && !v1 && (e0 || e1)) return false;
  if (v1 && (!v0 || other.sequence > record_.sequence)) record_ = other;
  // An unreadable newer reservation must not resurrect older pending time.
  // A valid in-flight record is safe to recover only by quarantining it.
  if (((e0 && !v0) || (e1 && !v1)) && !inFlightSeconds()) return false;
  healthy_ = true;
  return quarantineBatch();
}
bool WeReadTimeLedger::commit(const Record& next) {
  if (!healthy_ || record_.sequence == UINT64_MAX) return false;
  const uint64_t sequence = record_.sequence + 1;
  uint8_t b[kRecordSize] = {'W', 'R', 'T', 'M', 2};
  put(b + 8, sequence, 8);
  put(b + 16, next.acceptedSeconds, 8);
  put(b + 24, next.uncertainSeconds, 8);
  put(b + 32, next.unknownMs, 8);
  size_t off = 40;
  for (unsigned i = 0; i < kCapacity + kBatchHours; ++i, off += 8) {
    const Hour& h = i < kCapacity ? next.pending[i] : next.flight[i - kCapacity];
    put(b + off, h.start, 4);
    put(b + off + 4, h.amount, 4);
  }
  put(b + sizeof(b) - 4, checksum(b), 4);
  char name[184];
  snprintf(name, sizeof(name), "%s.%u", path_, unsigned(sequence & 1));
  {
    HalFile f;
    if (!Storage.openFileForWrite("WRTime", name, f) || f.write(b, sizeof(b)) != sizeof(b)) {
      healthy_ = false;
      return false;
    }
    f.flush();
  }
  // Read back the exact bytes; keep stack bounded (two 684-byte byte arrays).
  uint8_t checked[kRecordSize];
  {
    HalFile f;
    if (!Storage.openFileForRead("WRTime", name, f) || f.fileSize64() != sizeof(checked) ||
        f.read(checked, sizeof(checked)) != int(sizeof(checked)) || memcmp(b, checked, sizeof(b))) {
      healthy_ = false;
      return false;
    }
  }
  record_ = next;
  record_.sequence = sequence;
  dirty_ = false;
  return true;
}
void WeReadTimeLedger::add(uint32_t ms, uint32_t endEpoch) {
  if (!healthy_ || !ms) return;
  dirty_ = true;
  uint64_t end = uint64_t(endEpoch) * 1000;
  if (endEpoch < kMinEpoch || end < uint64_t(kMinEpoch) * 1000 + ms) {
    record_.unknownMs += ms;
    return;
  }
  uint64_t start = end - ms;
  while (start < end) {
    uint32_t hour = (start / 3600000) * 3600;
    uint64_t stop = std::min(end, (uint64_t(hour) + 3600) * 1000);
    Hour* bucket = nullptr;
    for (auto& h : record_.pending)
      if (h.start == hour) {
        bucket = &h;
        break;
      }
    if (!bucket)
      for (auto& h : record_.pending)
        if (!h.amount) {
          bucket = &h;
          h.start = hour;
          break;
        }
    // Subsecond leftovers must not permanently occupy all 64 slots after
    // many successful syncs. Preserve their amount as unreported before reuse.
    if (!bucket)
      for (auto& h : record_.pending)
        if (h.amount < 1000) {
          record_.unknownMs += h.amount;
          h = {hour, 0};
          bucket = &h;
          break;
        }
    uint32_t amount = stop - start;
    if (!bucket)
      record_.unknownMs += amount;
    else {
      uint32_t added = std::min(amount, 3600000 - bucket->amount);
      bucket->amount += added;
      record_.unknownMs += amount - added;
    }
    start = stop;
  }
}
bool WeReadTimeLedger::flush() { return healthy_ && (!dirty_ || commit(record_)); }
uint64_t WeReadTimeLedger::pendingSeconds() const {
  uint64_t v = 0;
  for (const auto& h : record_.pending) v += h.amount / 1000;
  return v;
}
uint32_t WeReadTimeLedger::inFlightSeconds() const {
  uint32_t v = 0;
  for (const auto& h : record_.flight) v += h.amount;
  return v;
}
unsigned WeReadTimeLedger::batchHours(Hour* out, unsigned capacity) const {
  unsigned n = 0;
  const bool reserved = inFlightSeconds() > 0;
  if (reserved) {
    for (const auto& h : record_.flight)
      if (h.amount && n < capacity) out[n++] = h;
  } else {
    for (const auto& h : record_.pending)
      if (h.amount >= 1000 && n < capacity && n < kBatchHours) out[n++] = {h.start, h.amount / 1000};
  }
  return n;
}
bool WeReadTimeLedger::prepareBatch() {
  if (!healthy_ || inFlightSeconds() || !pendingSeconds()) return false;
  Record next = record_;
  unsigned n = 0;
  for (auto& h : next.pending)
    if (h.amount >= 1000 && n < kBatchHours) {
      uint32_t sec = h.amount / 1000;
      next.flight[n++] = {h.start, sec};
      h.amount -= sec * 1000;
      if (!h.amount) h.start = 0;
    }
  return commit(next);
}
bool WeReadTimeLedger::acknowledgeBatch() {
  if (!inFlightSeconds()) return false;
  Record next = record_;
  next.acceptedSeconds += inFlightSeconds();
  for (auto& h : next.flight) h = {};
  return commit(next);
}
bool WeReadTimeLedger::quarantineBatch() {
  if (!inFlightSeconds()) return healthy_;
  Record next = record_;
  next.uncertainSeconds += inFlightSeconds();
  for (auto& h : next.flight) h = {};
  return commit(next);
}
