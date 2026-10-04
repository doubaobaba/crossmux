#pragma once

#include "WeReadHttpClient.h"
#include "WeReadNativeAuthProtocol.h"

namespace WeReadClient {
enum class Error;
}
namespace WeReadNativeAuth {
// Scoped, checked heap workspace. Never coexists with the native upload workspace.
class Login {
 public:
  enum class Event { None, QrReady, Scanned, Complete, Failed };
  enum class Failure { None, Expired, Declined, AccountMismatch };
  bool begin(const char* expectedVid, const char* bookId, bool forceLogin = false);
  Event step(WeReadClient::Error& error);
  const char* qrUrl() const { return qrUrl_; }
  Failure failure() const { return failure_; }
  bool readyToStep() const;

 private:
  enum class Phase { Load, Verify, Ticket, Qr, Poll, Exchange, Save, Done, Failed } phase_ = Phase::Load;
  WeReadHttpClient::Session session_;
  WeReadNativeProtocol::Credentials credentials_;
  Reply reply_;
  StreamingJsonParser parser_{reply_.callbacks()};
  char expectedVid_[64] = {}, bookId_[64] = {}, uuid_[128] = {}, qrUrl_[256] = {};
  char url_[512] = {}, body_[1024] = {};
  uint8_t io_[1024] = {};
  uint32_t started_ = 0, nextPoll_ = 0;
  bool forceLogin_ = false, scanned_ = false;
  Failure failure_ = Failure::None;
  WeReadClient::Error terminalError_{};
  bool load();
  bool save();
  void newDevice();
  WeReadClient::Error request(const char* url, bool authenticated = false, bool post = false, int timeout = 15000);
};
}  // namespace WeReadNativeAuth
