#pragma once

#ifdef ENABLE_CHINESE_VERSION
#include <WeReadTimeLedger.h>

class WeReadReadingTime {
 public:
  static WeReadReadingTime& instance();
  void begin(const char* bookId);
  void tick(bool visible);
  void interact();
  void pause();
  void resume();
  void end();
  bool storageFailed() const { return storageFailed_; }

 private:
  WeReadTimeLedger ledger_;
  WeReadReadingClock clock_;
  uint32_t checkpointAt_ = 0;
  bool active_ = false;
  bool paused_ = false;
  bool storageFailed_ = false;
};
#define WEREAD_TIME WeReadReadingTime::instance()
#endif
