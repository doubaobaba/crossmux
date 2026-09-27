#include "WeReadReadingTime.h"

#ifdef ENABLE_CHINESE_VERSION
#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <WeReadStore.h>

#include "util/TimeUtils.h"

WeReadReadingTime& WeReadReadingTime::instance() {
  static WeReadReadingTime tracker;
  return tracker;
}

void WeReadReadingTime::begin(const char* bookId) {
  end();
  if (!bookId || !bookId[0] || storageFailed_) return;
  // Session is 832 bytes: cold-path, checked heap allocation keeps the reader
  // stack small. Credentials are cleared and released before normal reading.
  auto session = makeUniqueNoThrow<WeReadStore::Session>();
  if (!session) {
    LOG_ERR("WRTime", "OOM: session");
    storageFailed_ = true;
    return;
  }
  const bool loggedIn = WeReadStore::loadSession(*session) && session->valid();
  if (loggedIn) {
    active_ = ledger_.open(session->vid, bookId);
    storageFailed_ = !active_;
  }
  session->clear();
  paused_ = false;
  checkpointAt_ = millis();
  clock_.resume(checkpointAt_);
}

void WeReadReadingTime::tick(const bool visible) {
  if (!active_ || paused_) return;
  const uint32_t now = millis();
  ledger_.add(clock_.tick(now, visible), TimeUtils::getCurrentValidTimestamp());
  if (now - checkpointAt_ >= 60000) {
    checkpointAt_ = now;
    if (!ledger_.flush()) {
      storageFailed_ = true;
      LOG_ERR("WRTime", "Checkpoint failed; time sync disabled");
    }
  }
}

void WeReadReadingTime::interact() {
  if (active_ && !paused_) clock_.interact();
}
void WeReadReadingTime::pause() {
  if (!active_) return;
  paused_ = true;
  if (!ledger_.flush()) storageFailed_ = true;
}
void WeReadReadingTime::resume() {
  if (!active_) return;
  paused_ = false;
  clock_.resume(millis());
}
void WeReadReadingTime::end() {
  if (active_ && !ledger_.flush()) storageFailed_ = true;
  active_ = false;
}
#endif
