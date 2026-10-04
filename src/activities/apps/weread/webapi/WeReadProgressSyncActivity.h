#pragma once

#include <WeReadNativeAuth.h>
#include <WeReadTimeLedger.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "activities/apps/weread/WeReadBackend.h"
#include "activities/apps/weread/WeReadProgressContext.h"

class Epub;
struct CrossPointPosition;

class WeReadProgressSyncActivity final : public Activity {
 public:
  WeReadProgressSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                             const char* bookId, const WeReadProgressContext& context);

  static WeReadProgressContext makeContext(const Epub& epub, const char* bookId, float localFraction,
                                           const CrossPointPosition& localPosition);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override;

 private:
  enum class State : uint8_t {
    WifiSelection,
    Starting,
    Authenticating,
    AuthQr,
    AuthScanned,
    AuthFailed,
    Syncing,
    ChoosingDirection,
    TimeStarting,
    TimeSyncing,
    TimeFailed,
    Success,
    Failed,
    LoginRequired,
  };

  enum class DirectionOption : uint8_t {
    ApplyRemote,
    UploadLocal,
  };

  State state_ = State::WifiSelection;
  WeReadClient::Operation operation_;
  WeReadTimeLedger timeLedger_;
  uint64_t acceptedAtStart_ = 0;
  bool timeReady_ = false;
  std::unique_ptr<WeReadNativeAuth::Login> nativeLogin_;
  WeReadNativeAuth::Login::Failure authFailure_ = WeReadNativeAuth::Login::Failure::None;
  WeReadNativeAuth::Login::Phase authPhase_ = WeReadNativeAuth::Login::Phase::Load;
  char authDiagnostic_[96] = {};
  bool nativeReady_ = false;
  bool nativeRecoveryAttempted_ = false;
  bool forceNativeLogin_ = false;
  char nativeQrUrl_[256] = {};
  WeReadClient::Error error_ = WeReadClient::Error::Ok;
  WeReadClient::ProgressSyncMode syncMode_ = WeReadClient::ProgressSyncMode::Compare;
  WeReadClient::ProgressSyncOutcome outcome_ = WeReadClient::ProgressSyncOutcome::Pending;
  DirectionOption selectedDirection_ = DirectionOption::ApplyRemote;
  std::string epubPath_;
  char bookId_[64] = {};
  WeReadClient::ProgressSyncInput input_;
  uint16_t localSpineIndex_ = 0;
  uint16_t localPageNumber_ = 0;
  uint16_t localPageCount_ = 0;
  float remoteFraction_ = 0.0f;
  bool uploadConflict_ = false;
  bool wifiActivated_ = false;
  bool radioStopped_ = false;
  std::atomic<bool> fullRefreshPending_{true};

  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void startSync();
  void startNativeLogin();
  void advanceNativeLogin();
  void advanceSync();
  void beginSelectedDirection();
  void startTimeSync();
  void applyRemoteProgress(const WeReadProtocol::RemoteProgress& remote);
  void returnToReader();
  const char* resultMessage() const;
  const char* errorMessage() const;
  const char* authPhaseMessage() const;
};
