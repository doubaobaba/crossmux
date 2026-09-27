#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "WeReadTimeLedger.h"

class WeReadTimeLedgerTest : public ::testing::Test {
 protected:
  std::filesystem::path root;
  void SetUp() override {
    char name[] = "/tmp/crossmux-time-XXXXXX";
    root = mkdtemp(name);
    setenv("CROSSPOINT_SIM_SD", root.c_str(), 1);
    ASSERT_TRUE(Storage.begin());
  }
  void TearDown() override {
    unsetenv("CROSSPOINT_SIM_SD");
    std::filesystem::remove_all(root);
  }
  std::filesystem::path slot(int number) {
    return root / ".crosspoint/weread-time-v2/123/456" += "." + std::to_string(number);
  }
};

TEST_F(WeReadTimeLedgerTest, OfflineSessionsSurviveRestartAndPreserveSubseconds) {
  WeReadTimeLedger first;
  ASSERT_TRUE(first.open("123", "456"));
  first.add(60900, 1700001000);
  ASSERT_TRUE(first.flush());
  WeReadTimeLedger next;
  ASSERT_TRUE(next.open("123", "456"));
  EXPECT_EQ(next.pendingSeconds(), 60);
  next.add(200, 1700001000);
  ASSERT_TRUE(next.flush());
  WeReadTimeLedger reboot;
  ASSERT_TRUE(reboot.open("123", "456"));
  EXPECT_EQ(reboot.pendingSeconds(), 61);
}

TEST_F(WeReadTimeLedgerTest, AccountsAndBooksDoNotShareTime) {
  WeReadTimeLedger a, b;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(60000, 1700001000);
  ASSERT_TRUE(a.flush());
  ASSERT_TRUE(b.open("999", "456"));
  EXPECT_EQ(b.pendingSeconds(), 0);
  ASSERT_TRUE(b.open("123", "789"));
  EXPECT_EQ(b.pendingSeconds(), 0);
}

TEST_F(WeReadTimeLedgerTest, SuccessfulBatchesNeverReplayAndKeepRemainder) {
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(65123, 1700001000);
  ASSERT_TRUE(a.prepareBatch());
  EXPECT_EQ(a.inFlightSeconds(), 65);
  EXPECT_EQ(a.pendingSeconds(), 0);
  EXPECT_FALSE(a.prepareBatch());
  ASSERT_TRUE(a.acknowledgeBatch());
  EXPECT_FALSE(a.prepareBatch());
  WeReadTimeLedger reboot;
  ASSERT_TRUE(reboot.open("123", "456"));
  EXPECT_EQ(reboot.acceptedSeconds(), 65);
  EXPECT_EQ(reboot.pendingSeconds(), 0);
  reboot.add(877, 1700001001);
  EXPECT_EQ(reboot.pendingSeconds(), 1);
}
TEST_F(WeReadTimeLedgerTest, CrashAfterReservationQuarantinesInsteadOfResending) {
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(90000, 1700001000);
  ASSERT_TRUE(a.prepareBatch());
  WeReadTimeLedger reboot;
  ASSERT_TRUE(reboot.open("123", "456"));
  EXPECT_EQ(reboot.pendingSeconds(), 0);
  EXPECT_EQ(reboot.uncertainSeconds(), 90);
  EXPECT_EQ(reboot.inFlightSeconds(), 0);
  EXPECT_EQ(reboot.acceptedSeconds(), 0);
  EXPECT_FALSE(reboot.prepareBatch());
}
TEST_F(WeReadTimeLedgerTest, TornAcknowledgementRecoversReservedBatchConservatively) {
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(90000, 1700001000);
  ASSERT_TRUE(a.flush());
  ASSERT_TRUE(a.prepareBatch());
  std::ofstream(slot(1), std::ios::binary | std::ios::trunc) << "WRT";
  WeReadTimeLedger reboot;
  ASSERT_TRUE(reboot.open("123", "456"));
  EXPECT_EQ(reboot.pendingSeconds(), 0);
  EXPECT_EQ(reboot.uncertainSeconds(), 90);
}
TEST_F(WeReadTimeLedgerTest, HourBoundaryAndIdleGapsPreserveActualIntervals) {
  constexpr uint32_t h = 1699999200;
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(20000, h + 3610);
  a.add(100000, h + 7300);
  WeReadTimeLedger::Hour hours[16];
  ASSERT_EQ(a.batchHours(hours, 16), 3);
  EXPECT_EQ(hours[0].start, h);
  EXPECT_EQ(hours[0].amount, 10);
  EXPECT_EQ(hours[1].start, h + 3600);
  EXPECT_EQ(hours[1].amount, 10);
  EXPECT_EQ(hours[2].start, h + 7200);
  EXPECT_EQ(hours[2].amount, 100);
  EXPECT_EQ(a.pendingSeconds(), 120);
}
TEST_F(WeReadTimeLedgerTest, MidnightAndInvalidClockNeverMoveUnknownTimeToToday) {
  constexpr uint32_t midnight = 1790524800;
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(60000, 0);
  a.add(60000, midnight + 30);
  ASSERT_TRUE(a.flush());
  WeReadTimeLedger b;
  ASSERT_TRUE(b.open("123", "456"));
  WeReadTimeLedger::Hour hours[16];
  ASSERT_EQ(b.batchHours(hours, 16), 2);
  EXPECT_EQ(hours[0].start, midnight - 3600);
  EXPECT_EQ(hours[0].amount, 30);
  EXPECT_EQ(hours[1].start, midnight);
  EXPECT_EQ(hours[1].amount, 30);
  EXPECT_EQ(b.uncertainSeconds(), 60);
  EXPECT_EQ(b.pendingSeconds(), 60);
}
TEST_F(WeReadTimeLedgerTest, MultiDayQueueIsBoundedAndBatchesAreImmediate) {
  constexpr uint32_t h = 1699999200;
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  for (unsigned i = 0; i < 65; ++i) a.add(60000, h + i * 3600 + 60);
  EXPECT_EQ(a.pendingSeconds(), 64 * 60);
  EXPECT_EQ(a.uncertainSeconds(), 60);
  for (unsigned i = 0; i < 4; ++i) {
    ASSERT_TRUE(a.prepareBatch());
    EXPECT_EQ(a.inFlightSeconds(), 16 * 60);
    ASSERT_TRUE(a.acknowledgeBatch());
  }
  EXPECT_EQ(a.pendingSeconds(), 0);
  EXPECT_EQ(a.acceptedSeconds(), 64 * 60);
}

TEST_F(WeReadTimeLedgerTest, CorruptLedgerAndInvalidIdentifiersFailClosed) {
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(1000, 1700001000);
  ASSERT_TRUE(a.flush());
  std::ofstream(slot(1), std::ios::binary | std::ios::trunc) << "broken";
  EXPECT_FALSE(a.open("123", "456"));
  EXPECT_FALSE(a.prepareBatch());
  EXPECT_FALSE(a.open("../123", "456"));
  EXPECT_FALSE(a.open("123", "../456"));
  EXPECT_FALSE(a.open("123", "MP_WXS_123"));
}

TEST_F(WeReadTimeLedgerTest, FailedReservationPreventsPosting) {
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(60000, 1700001000);
  ASSERT_TRUE(a.flush());
  std::filesystem::create_directory(slot(0));  // opening alternate slot for write fails
  EXPECT_FALSE(a.prepareBatch());
  EXPECT_FALSE(a.healthy());
  EXPECT_EQ(a.inFlightSeconds(), 0);
  EXPECT_EQ(a.pendingSeconds(), 60);
}

TEST(WeReadReadingClockTest, CountsVisibleTimeAndExcludesMenusAndSleep) {
  WeReadReadingClock clock;
  clock.resume(1000);
  EXPECT_EQ(clock.tick(31000, true), 30000);
  EXPECT_EQ(clock.tick(91000, false), 0);
  EXPECT_EQ(clock.tick(121000, true), 30000);
  clock.resume(500000);  // child activity or deep sleep must not become reading
  EXPECT_EQ(clock.tick(510000, true), 10000);
}

TEST(WeReadReadingClockTest, CapsIdleTimeAndHandlesMillisWrap) {
  WeReadReadingClock clock;
  clock.resume(0);
  for (int i = 1; i <= 5; ++i) EXPECT_EQ(clock.tick(i * 60000, true), 60000);
  EXPECT_EQ(clock.tick(360000, true), 0);
  clock.interact();
  EXPECT_EQ(clock.tick(390000, true), 30000);
  clock.resume(UINT32_MAX - 99);
  EXPECT_EQ(clock.tick(900, true), 1000);
}

TEST_F(WeReadTimeLedgerTest, CorruptedReservationCannotResurrectPendingTime) {
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  a.add(60000, 1700001000);
  ASSERT_TRUE(a.flush());
  ASSERT_TRUE(a.prepareBatch());
  std::ofstream(slot(0), std::ios::binary | std::ios::trunc) << "bad";
  WeReadTimeLedger reboot;
  EXPECT_FALSE(reboot.open("123", "456"));
  EXPECT_FALSE(reboot.prepareBatch());
}

TEST_F(WeReadTimeLedgerTest, FractionalRemaindersDoNotBlockFutureHours) {
  WeReadTimeLedger a;
  ASSERT_TRUE(a.open("123", "456"));
  for (unsigned i = 0; i < 100; ++i) {
    a.add(1500, 1699999200 + i * 3600 + 2);
    ASSERT_TRUE(a.prepareBatch());
    ASSERT_TRUE(a.acknowledgeBatch());
  }
  EXPECT_EQ(a.acceptedSeconds(), 100);
  EXPECT_EQ(a.pendingSeconds(), 0);
  EXPECT_EQ(a.uncertainSeconds(), 18);  // 36 evicted half-seconds, preserved locally.
}
