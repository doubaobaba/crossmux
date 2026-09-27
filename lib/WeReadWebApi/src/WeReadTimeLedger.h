#pragma once

#include <cstdint>

// Version 2: bounded hourly outbox, independent of editable reading statistics.
class WeReadTimeLedger {
 public:
  static constexpr unsigned kCapacity = 64;
  static constexpr unsigned kBatchHours = 16;
  struct Hour {
    uint32_t start = 0;
    uint32_t amount = 0;
  };
  bool open(const char* account, const char* book);
  void add(uint32_t milliseconds, uint32_t endEpochSeconds = 0);
  bool flush();
  bool prepareBatch();
  bool acknowledgeBatch();
  bool quarantineBatch();
  uint64_t pendingSeconds() const;
  uint64_t acceptedSeconds() const { return record_.acceptedSeconds; }
  uint64_t uncertainSeconds() const { return record_.uncertainSeconds + record_.unknownMs / 1000 + inFlightSeconds(); }
  uint32_t inFlightSeconds() const;
  unsigned batchHours(Hour* output, unsigned capacity) const;
  const char* account() const { return account_; }
  bool healthy() const { return healthy_; }

 private:
  struct Record {
    uint64_t sequence = 0;
    uint64_t acceptedSeconds = 0;
    uint64_t uncertainSeconds = 0;
    uint64_t unknownMs = 0;
    Hour pending[kCapacity];   // milliseconds, at most one hour each
    Hour flight[kBatchHours];  // seconds, reserved before a non-idempotent POST
  } record_;
  char path_[176] = {};
  char account_[64] = {};
  bool healthy_ = false;
  bool dirty_ = false;
  bool readSlot(unsigned slot, Record& record, bool& exists) const;
  bool commit(const Record& next);
};

// Unsigned subtraction deliberately handles millis() wrap. A page left open
// unattended can contribute at most five minutes until the next interaction.
class WeReadReadingClock {
 public:
  static constexpr uint32_t kIdleLimitMs = 5 * 60 * 1000;
  void resume(uint32_t now) {
    last_ = now;
    allowance_ = kIdleLimitMs;
  }
  uint32_t tick(uint32_t now, bool visible) {
    const uint32_t elapsed = now - last_;
    last_ = now;
    if (!visible) return 0;
    const uint32_t credited = elapsed < allowance_ ? elapsed : allowance_;
    allowance_ -= credited;
    return credited;
  }
  void interact() { allowance_ = kIdleLimitMs; }

 private:
  uint32_t last_ = 0;
  uint32_t allowance_ = 0;
};
