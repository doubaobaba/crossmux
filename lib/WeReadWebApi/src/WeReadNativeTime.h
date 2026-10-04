#pragma once
#include <StreamingJsonParser.h>

#include "WeReadHttpClient.h"
#include "WeReadNativeProtocol.h"
namespace WeReadClient {
enum class Error;
}
namespace WeReadNativeTime {
const char* rootCA();
bool hash256(const uint8_t* data, size_t size, char out[65]);
struct Reply {
  WeReadNativeProtocol::Credentials credentials;
  WeReadNativeProtocol::Position position;
  char bookId[64] = {}, configToken[256] = {}, guestToken[128] = {}, key[64] = {};
  uint64_t readingTime = 0;
  uint32_t version = 0;
  int errorCode = 0, depth = 0, bookDepth = 0, featureDepth = 0;
  unsigned positionFields = 0;
  bool closed = false, invalid = false, succeeded = false, hasReadingTime = false;
  JsonCallbacks callbacks();
};
// One bounded allocation per explicit sync; freed before returning to the reader.
// Per-request stages allow cancellation between requests. No background service.
class Upload {
 public:
  ~Upload();
  // The ledger must outlive this workspace, including cancellation.
  bool begin(const char* bookId, WeReadTimeLedger& ledger);
  bool step(WeReadClient::Error& error);  // true = finished (success or failure)
  void diagnosticCode(char* out, size_t capacity) const;

 private:
  enum class Phase { Load, Config, Feature, Info, Progress, Post, Verify, Done } phase_ = Phase::Load;
  WeReadHttpClient::Session session_;
  WeReadNativeProtocol::Credentials credentials_;
  WeReadNativeProtocol::Position position_;
  Reply reply_;
  WeReadTimeLedger* ledger_ = nullptr;
  WeReadTimeLedger::Hour hours_[WeReadTimeLedger::kBatchHours];
  char bookId_[64] = {}, token_[256] = {}, guest_[128] = "5ecdcfd7f";
  char body_[4096] = {};
  uint8_t io_[1024] = {};
  uint64_t before_ = 0;
  uint32_t version_ = 0;
  WeReadHttpClient::Diagnostic diagnostic_;
  WeReadHttpClient::Result transportResult_ = WeReadHttpClient::Result::Ok;
  Phase failurePhase_ = Phase::Load;
  int httpStatus_ = -1;
  size_t received_ = 0;
  uint32_t nextVerify_ = 0;
  uint8_t verifyAttempts_ = 0;
  bool parserError_ = false, postAcknowledged_ = false, verifyWaiting_ = false;
  WeReadClient::Error request(const char* path, bool post = false);
  bool loadCredentials();
  bool resolvePosition();
  void saveDiagnostic(WeReadClient::Error error);
};
}  // namespace WeReadNativeTime
