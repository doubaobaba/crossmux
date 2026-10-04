#include "WeReadNativeAuth.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_random.h>

#include <cstdio>
#include <cstring>

#include "../../../src/util/TimeUtils.h"
#include "WeReadClient.h"
#include "WeReadNativeTime.h"
#include "WeReadStore.h"

namespace WeReadNativeAuth {
using WeReadClient::Error;
using WeReadNativeProtocol::token;
namespace {
constexpr const char* kSession = "/.crosspoint/weread/native-session.json";
constexpr const char* kBackup = "/.crosspoint/weread/native-session.json.bak";
constexpr const char* kPart = "/.crosspoint/weread/native-session.json.part";
constexpr uint32_t kQrLifetime = 300000;
bool fits(int n, size_t capacity) { return n >= 0 && static_cast<size_t>(n) < capacity; }
bool urlToken(const char* value) {
  if (!token(value)) return false;
  return !strpbrk(value, "+/=");
}
}  // namespace
bool Login::begin(const char* expectedVid, const char* bookId, bool forceLogin) {
  if (!token(expectedVid, true) || strlen(expectedVid) >= sizeof(expectedVid_) || !token(bookId, true) ||
      strlen(bookId) >= sizeof(bookId_))
    return false;
  strcpy(expectedVid_, expectedVid);
  strcpy(bookId_, bookId);
  forceLogin_ = forceLogin;
  return true;
}
bool Login::load() {
  // Recover the previous complete session if power failed between the two renames.
  if (!Storage.exists(kSession) && Storage.exists(kBackup) && !Storage.rename(kBackup, kSession)) return false;
  reply_ = {};
  parser_.reset();
  HalFile file;
  if (!Storage.openFileForRead("WRAuth", kSession, file) || file.fileSize64() > 4096) return false;
  int n = 0;
  while ((n = file.read(io_, sizeof(io_))) > 0) parser_.feed(reinterpret_cast<char*>(io_), n);
  parser_.feed(" ", 1);
  if (n < 0 || parser_.hasError() || reply_.invalid || !reply_.closed ||
      !validCredentials(reply_.credentials, expectedVid_))
    return false;
  credentials_ = reply_.credentials;
  return true;
}
bool Login::save() {
  if (!sessionJson(credentials_, expectedVid_, body_, sizeof(body_)) || !WeReadStore::ensureRoot()) return false;
  {
    HalFile file;
    if (!Storage.openFileForWrite("WRAuth", kPart, file) ||
        file.write(reinterpret_cast<const uint8_t*>(body_), strlen(body_)) != strlen(body_))
      return false;
    file.flush();
  }
  {
    HalFile file;
    const size_t size = strlen(body_);
    if (!Storage.openFileForRead("WRAuth", kPart, file) || file.fileSize64() != size ||
        file.read(io_, size) != static_cast<int>(size) || memcmp(io_, body_, size))
      return false;
  }
  if (!WeReadStore::atomicReplace(kPart, kSession)) return false;
  // Re-read before telling the user the login is saved. Upload uses this same SD path.
  return load();
}
void Login::newDevice() {
  credentials_ = {};
  strcpy(credentials_.deviceId, "eink334691225");
  for (unsigned i = 12; i < 31; ++i) credentials_.deviceId[i] = '0' + esp_random() % 10;
  strcpy(credentials_.installId, "eink31");
  for (unsigned i = 6; i < 32; ++i) credentials_.installId[i] = '0' + esp_random() % 10;
}
Error Login::request(const char* url, bool authenticated, bool post, int timeout) {
  const WeReadHttpClient::Header headers[] = {
      {"baseapi", "30"},
      {"appver", "2.1.2.10245900"},
      {"basever", "2.1.2.10245900"},
      {"osver", "11"},
      {"channelId", "900"},
      {"User-Agent", "WeRead/2.1.2 WRBrand/Onyx wr_eink Dalvik/2.1.0 (Linux; U; Android 11; BOOX Build/onyx)"},
      {"Content-Type", "application/json"},
      {"vid", credentials_.vid},
      {"accessToken", credentials_.accessToken}};
  WeReadHttpClient::RequestOptions options;
  options.method = post ? "POST" : "GET";
  options.headers = headers;
  options.headerCount = authenticated ? 9 : 7;
  options.body = post ? reinterpret_cast<uint8_t*>(body_) : nullptr;
  options.bodySize = post ? strlen(body_) : 0;
  options.timeoutMs = timeout;
  options.redactUrl = true;
  options.rootCA = WeReadNativeTime::rootCA();
  options.readBuffer = io_;
  options.readBufferSize = sizeof(io_);
  options.diagnostic = &diagnostic_;
  reply_ = {};
  parser_.reset();
  httpStatus_ = 0;
  received_ = 0;
  transportResult_ = WeReadHttpClient::request(
      session_, url, options,
      [&](const uint8_t* data, size_t length) {
        received_ += length;
        if (received_ > 128 * 1024) return false;
        parser_.feed(reinterpret_cast<const char*>(data), length);
        return !parser_.hasError() && !reply_.invalid;
      },
      {}, httpStatus_);
  parser_.feed(" ", 1);
  if (httpStatus_ == 401 || httpStatus_ == 403) {
    session_.reset();
    return Error::SessionExpired;
  }
  if (transportResult_ != WeReadHttpClient::Result::Ok) {
    session_.reset();
    return transportResult_ == WeReadHttpClient::Result::Aborted ? Error::Protocol : Error::Network;
  }
  if (!parser_.hasError() && !reply_.invalid && reply_.closed &&
      (reply_.errorCode == -2010 || reply_.errorCode == -2012))
    return Error::SessionExpired;
  if (httpStatus_ != 200 || parser_.hasError() || reply_.invalid || !reply_.closed || reply_.errorCode)
    return Error::Protocol;
  return Error::Ok;
}
void Login::diagnosticCode(char* out, size_t capacity) const {
  snprintf(out, capacity, "A%u N%u T%u:%d H%d E%d", unsigned(failurePhase_), unsigned(diagnostic_.stage),
           unsigned(diagnostic_.tlsStage), diagnostic_.tlsError, httpStatus_, reply_.errorCode);
}
void Login::saveDiagnostic(Error error) {
  // Reuse the existing request workspace. One small overwrite per terminal
  // failure; no writes on polling timeouts, and no credentials/account/book IDs.
  const int size = snprintf(
      body_, sizeof(body_),
      "version=%s\nutc=%lu\nauth_phase=%u\nerror=%u\nhttp_stage=%u\nhttp_status=%d\n"
      "transport_result=%u\ntls_stage=%u\ntls_error=%d\nfirst_tls_error=%d\n"
      "received=%u\napi_error=%d\njson_error=%u\ninvalid=%u\nclosed=%u\n"
      "free_before=%lu\nlargest_before=%lu\nfree_after=%lu\nlargest_after=%lu\n",
      CROSSPOINT_VERSION, static_cast<unsigned long>(TimeUtils::getCurrentValidTimestamp()), unsigned(failurePhase_),
      unsigned(error), unsigned(diagnostic_.stage), httpStatus_, unsigned(transportResult_),
      unsigned(diagnostic_.tlsStage), diagnostic_.tlsError, diagnostic_.firstTlsError, unsigned(received_),
      reply_.errorCode, parser_.hasError(), reply_.invalid, reply_.closed,
      static_cast<unsigned long>(diagnostic_.freeBefore), static_cast<unsigned long>(diagnostic_.largestBefore),
      static_cast<unsigned long>(diagnostic_.freeAfter), static_cast<unsigned long>(diagnostic_.largestAfter));
  if (!fits(size, sizeof(body_)) || !WeReadStore::ensureRoot()) return;
  HalFile file;
  if (!Storage.openFileForWrite("WRAuth", "/.crosspoint/weread/native-auth-error.txt", file)) return;
  if (file.write(reinterpret_cast<const uint8_t*>(body_), size) != static_cast<size_t>(size))
    LOG_ERR("WRAuth", "Diagnostic write failed");
  file.flush();
}
bool Login::readyToStep() const {
  return phase_ != Phase::Poll || static_cast<int32_t>(millis() - nextPoll_) >= 0 ||
         static_cast<uint32_t>(millis() - started_) >= kQrLifetime;
}
Login::Event Login::step(Error& error) {
  error = Error::Ok;
  if (!readyToStep()) return Event::None;
  switch (phase_) {
    case Phase::Load:
      if (!TimeUtils::getCurrentValidTimestamp()) {
        error = Error::Clock;
        break;
      }
      phase_ = !forceLogin_ && load() ? Phase::Verify : Phase::Ticket;
      return Event::None;
    case Phase::Verify:
      snprintf(url_, sizeof(url_), "https://i.weread.qq.com/book/getProgress?bookId=%s", bookId_);
      error = request(url_, true);
      if (error == Error::SessionExpired) {
        error = Error::Ok;
        session_.reset();
        phase_ = Phase::Ticket;
        return Event::None;
      }
      if (error == Error::Ok && strcmp(reply_.bookId, bookId_)) error = Error::Protocol;
      if (error == Error::Ok) {
        phase_ = Phase::Done;
        session_.reset();
        return Event::Complete;
      }
      break;
    case Phase::Ticket:
      newDevice();
      error = request("https://i.weread.qq.com/wxticket?nonceStr=weread");
      if (error != Error::Ok) break;
      if (!token(reply_.timestamp, true) || !urlToken(reply_.signature) ||
          !fits(snprintf(url_, sizeof(url_),
                         "https://open.weixin.qq.com/connect/sdk/qrconnect?"
                         "appid=wxab9b71ad2b90ff34&noncestr=weread&timestamp=%s&scope=snsapi_userinfo%%2Csnsapi_"
                         "timeline%%2Csnsapi_friend&signature=%s",
                         reply_.timestamp, reply_.signature),
                sizeof(url_))) {
        error = Error::Protocol;
        break;
      }
      phase_ = Phase::Qr;
      return Event::None;
    case Phase::Qr:
      error = request(url_);
      if (error != Error::Ok) break;
      if (!urlToken(reply_.uuid)) {
        error = Error::Protocol;
        break;
      }
      strcpy(uuid_, reply_.uuid);
      snprintf(qrUrl_, sizeof(qrUrl_), "https://open.weixin.qq.com/connect/confirm?uuid=%s", uuid_);
      started_ = millis();
      nextPoll_ = started_ + 1500;
      phase_ = Phase::Poll;
      return Event::QrReady;
    case Phase::Poll:
      if (static_cast<uint32_t>(millis() - started_) >= kQrLifetime) {
        failure_ = Failure::Expired;
        error = Error::LoginFailed;
        break;
      }
      // A bounded long poll, only while this foreground QR screen is open.
      snprintf(url_, sizeof(url_), "https://long.open.weixin.qq.com/connect/l/qrconnect?f=json&uuid=%s%s", uuid_,
               scanned_ ? "&last=404" : "");
      error = request(url_, false, false, 6000);
      nextPoll_ = millis() + 1500;
      if (error == Error::Network && WeReadHttpClient::networkReady()) {
        error = Error::Ok;
        return Event::None;
      }
      if (error != Error::Ok) break;
      if (!reply_.hasQrStatus) {
        error = Error::Protocol;
        break;
      }
      if (reply_.qrStatus == 408) return Event::None;
      if (reply_.qrStatus == 404) {
        if (scanned_) return Event::None;
        scanned_ = true;
        return Event::Scanned;
      }
      if (reply_.qrStatus == 405) {
        if (!loginBody(credentials_, reply_.code,
                       uint64_t(TimeUtils::getCurrentValidTimestamp()) * 1000 + millis() % 1000, esp_random() % 1000,
                       WeReadNativeTime::hash256, body_, sizeof(body_))) {
          error = Error::Protocol;
          break;
        }
        phase_ = Phase::Exchange;
        return Event::None;
      }
      if (reply_.qrStatus == 402 || reply_.qrStatus == 403) {
        failure_ = reply_.qrStatus == 402 ? Failure::Expired : Failure::Declined;
        error = Error::LoginFailed;
      } else
        error = Error::Protocol;
      break;
    case Phase::Exchange:
      error = request("https://i.weread.qq.com/login", false, true);
      if (error != Error::Ok) break;
      strcpy(reply_.credentials.deviceId, credentials_.deviceId);
      strcpy(reply_.credentials.installId, credentials_.installId);
      if (token(reply_.credentials.vid, true) && strcmp(reply_.credentials.vid, expectedVid_)) {
        failure_ = Failure::AccountMismatch;
        error = Error::LoginFailed;
        break;
      }
      if (!validCredentials(reply_.credentials, expectedVid_)) {
        error = Error::Protocol;
        break;
      }
      credentials_ = reply_.credentials;
      phase_ = Phase::Save;
      return Event::None;
    case Phase::Save:
      if (!save()) {
        error = Error::SdCard;
        break;
      }
      phase_ = Phase::Done;
      session_.reset();
      return Event::Complete;
    case Phase::Done:
      return Event::Complete;
    case Phase::Failed:
      error = terminalError_;
      return Event::Failed;
  }
  session_.reset();
  failurePhase_ = phase_;
  saveDiagnostic(error);
  phase_ = Phase::Failed;
  terminalError_ = error;
  return Event::Failed;
}
static_assert(sizeof(Login) <= 8 * 1024, "Native login fixed workspace exceeds 8 KiB");
}  // namespace WeReadNativeAuth
