#include "WeReadNativeTime.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

#include "../../../src/util/TimeUtils.h"
#include "WeReadClient.h"
#include "WeReadStore.h"

namespace WeReadNativeTime {
using WeReadClient::Error;
using namespace WeReadNativeProtocol;
namespace {
// DigiCert Global Root G2 from the pinned ESP-IDF Mozilla CA bundle.
// The observed official i.weread.qq.com chain terminates at this root.
constexpr char kRootCA[] = R"PEM(-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBhMQswCQYDVQQG
EwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3d3cuZGlnaWNlcnQuY29tMSAw
HgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBHMjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUx
MjAwMDBaMGExCzAJBgNVBAYTAlVTMRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3
dy5kaWdpY2VydC5jb20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkq
hkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI2/Ou8jqJ
kTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx1x7e/dfgy5SDN67sH0NO
3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQq2EGnI/yuum06ZIya7XzV+hdG82MHauV
BJVJ8zUtluNJbd134/tJS7SsVQepj5WztCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyM
UNGPHgm+F6HmIcr9g+UQvIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQAB
o0IwQDAPBgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV5uNu
5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY1Yl9PMWLSn/pvtsr
F9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4NeF22d+mQrvHRAiGfzZ0JFrabA0U
WTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NGFdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBH
QRFXGU7Aj64GxJUTFy8bJZ918rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/
iyK5S9kJRaTepLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)PEM";
constexpr const char* kSession = "/.crosspoint/weread/native-session.json";
bool copy(char* out, size_t capacity, const char* v, size_t n) {
  if (n >= capacity || memchr(v, 0, n)) return false;
  memcpy(out, v, n);
  out[n] = 0;
  return true;
}
bool integer(const char* v, size_t n, uint64_t& out) {
  out = 0;
  if (!n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (v[i] < '0' || v[i] > '9' || out > (UINT64_MAX - 9) / 10) return false;
    out = out * 10 + (v[i] - '0');
  }
  return true;
}
void onKey(void* ctx, const char* v, size_t n) {
  auto& r = *static_cast<Reply*>(ctx);
  if (!copy(r.key, sizeof(r.key), v, n)) r.key[0] = 0;
}
void value(void* ctx, const char* v, size_t n) {
  auto& r = *static_cast<Reply*>(ctx);
  const char* k = r.key;
  char* target = nullptr;
  size_t cap = 0;
#define TARGET(name, field) \
  if (!strcmp(k, name)) {   \
    target = field;         \
    cap = sizeof(field);    \
  }
  if (r.depth == 1) {
    TARGET("vid", r.credentials.vid)
    TARGET("accessToken", r.credentials.accessToken)
    TARGET("refreshToken", r.credentials.refreshToken)
    TARGET("deviceId", r.credentials.deviceId)
    TARGET("installId", r.credentials.installId)
    TARGET("bookId", r.bookId)
    TARGET("token", r.configToken)
  }
  if (r.featureDepth && r.depth == r.featureDepth) {
    TARGET("guest_token", r.guestToken)
  }
#undef TARGET
  if (target) {
    if (!copy(target, cap, v, n)) r.invalid = true;
    return;
  }
  if (r.depth == 1 && (!strcmp(k, "errCode") || !strcmp(k, "errcode"))) {
    uint64_t code = 0;
    bool negative = n && *v == '-';
    if (!integer(v + negative, n - negative, code) || code > INT32_MAX)
      r.invalid = true;
    else
      r.errorCode = negative ? -int(code) : int(code);
    return;
  }
  if (r.depth == 1 && !strcmp(k, "succ")) {
    r.succeeded = n == 1 && *v == '1';
    return;
  }
  uint64_t number = 0;
  if (r.depth == 1 && !strcmp(k, "version")) {
    if (!integer(v, n, number) || number > UINT32_MAX)
      r.invalid = true;
    else
      r.version = number;
  }
  if (r.bookDepth && r.depth == r.bookDepth) {
    if (!strcmp(k, "readingTime")) {
      r.hasReadingTime = integer(v, n, r.readingTime);
      if (!r.hasReadingTime) r.invalid = true;
      return;
    }
    uint32_t* field = nullptr;
    unsigned bit = 0;
    if (!strcmp(k, "chapterUid")) {
      field = &r.position.chapterUid;
      bit = 1;
    }
    if (!strcmp(k, "chapterOffset")) {
      field = &r.position.chapterOffset;
      bit = 2;
    }
    if (!strcmp(k, "progress")) {
      field = &r.position.progress;
      bit = 4;
    }
    if (!strcmp(k, "currentProgress")) {
      field = &r.position.currentProgress;
      bit = 8;
    }
    if (!strcmp(k, "chapterIdx")) {
      field = &r.position.chapterIdx;
      bit = 16;
    }
    if (field) {
      if (!integer(v, n, number) || number > UINT32_MAX)
        r.invalid = true;
      else {
        *field = number;
        r.positionFields |= bit;
      }
    }
  }
}
void start(void* ctx) {
  auto& r = *static_cast<Reply*>(ctx);
  if (r.closed) r.invalid = true;
  if (r.depth == 1 && !strcmp(r.key, "book")) r.bookDepth = 2;
  if (r.depth == 1 && !strcmp(r.key, "feature")) r.featureDepth = 2;
  ++r.depth;
}
void end(void* ctx) {
  auto& r = *static_cast<Reply*>(ctx);
  if (r.depth == r.bookDepth) r.bookDepth = 0;
  if (r.depth == r.featureDepth) r.featureDepth = 0;
  if (--r.depth == 0) r.closed = true;
}
void chunks(void* ctx, const char*, size_t, bool) {
  auto& r = *static_cast<Reply*>(ctx);
  // Large book descriptions are ignored; selected protocol/auth values must fit.
  if (r.depth == 1 &&
      (!strcmp(r.key, "token") || !strcmp(r.key, "vid") || !strcmp(r.key, "bookId") || !strcmp(r.key, "accessToken") ||
       !strcmp(r.key, "refreshToken") || !strcmp(r.key, "deviceId") || !strcmp(r.key, "installId")))
    r.invalid = true;
  if (r.featureDepth && !strcmp(r.key, "guest_token")) r.invalid = true;
}
bool sha256(const uint8_t* data, size_t size, char out[65]) {
  uint8_t digest[32];
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0 && mbedtls_sha256_update(&context, data, size) == 0 &&
            mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  if (!ok) return false;
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    out[i * 2] = hex[digest[i] >> 4];
    out[i * 2 + 1] = hex[digest[i] & 15];
  }
  out[64] = 0;
  return true;
}
}  // namespace
const char* rootCA() { return kRootCA; }
bool hash256(const uint8_t* data, size_t size, char out[65]) { return sha256(data, size, out); }
JsonCallbacks Reply::callbacks() {
  return {this, onKey, value, value, nullptr, nullptr, start, end, start, end, chunks};
}
bool Upload::begin(const char* bookId, WeReadTimeLedger& ledger) {
  if (!token(bookId, true) || strlen(bookId) >= sizeof(bookId_) || !ledger.healthy() || !ledger.pendingSeconds())
    return false;
  strcpy(bookId_, bookId);
  ledger_ = &ledger;
  return true;
}
bool Upload::loadCredentials() {
  reply_ = {};
  StreamingJsonParser parser(reply_.callbacks());
  HalFile f;
  if (!Storage.openFileForRead("WRNative", kSession, f) || f.fileSize64() > 4096) return false;
  int n = 0;
  while ((n = f.read(io_, sizeof(io_))) > 0) parser.feed(reinterpret_cast<char*>(io_), n);
  parser.feed(" ", 1);
  if (n < 0 || parser.hasError() || reply_.invalid || !reply_.closed) return false;
  credentials_ = reply_.credentials;
  return token(credentials_.vid, true) && !strcmp(credentials_.vid, ledger_->account()) &&
         token(credentials_.accessToken) && token(credentials_.deviceId) && token(credentials_.installId);
}
Error Upload::request(const char* path, bool post) {
  char url[192];
  int n = snprintf(url, sizeof(url), "https://i.weread.qq.com%s", path);
  if (n < 0 || size_t(n) >= sizeof(url)) return Error::Protocol;
  const WeReadHttpClient::Header headers[] = {
      {"baseapi", "30"},
      {"appver", "2.1.2.10245900"},
      {"basever", "2.1.2.10245900"},
      {"osver", "11"},
      {"channelId", "900"},
      {"User-Agent", "WeRead/2.1.2 WRBrand/Onyx wr_eink Dalvik/2.1.0 (Linux; U; Android 11; BOOX Build/onyx)"},
      {"vid", credentials_.vid},
      {"accessToken", credentials_.accessToken},
      {"Content-Type", "application/json"}};
  WeReadHttpClient::RequestOptions options;
  options.method = post ? "POST" : "GET";
  options.headers = headers;
  options.headerCount = sizeof(headers) / sizeof(headers[0]);
  options.body = post ? reinterpret_cast<uint8_t*>(body_) : nullptr;
  options.bodySize = post ? strlen(body_) : 0;
  options.timeoutMs = 15000;
  options.rootCA = kRootCA;
  options.redactUrl = true;
  options.readBuffer = io_;
  options.readBufferSize = sizeof(io_);
  options.diagnostic = &diagnostic_;
  if (post) {
    // Reserve durably immediately before the first HTTP byte. A failed wolfSSL
    // handshake leaves the hours pending; a partial write remains uncertain.
    options.beforeSend = [](void* context) { return static_cast<WeReadTimeLedger*>(context)->prepareBatch(); };
    options.beforeSendContext = ledger_;
  }
  reply_ = {};
  StreamingJsonParser parser(reply_.callbacks());
  httpStatus_ = -1;
  received_ = 0;
  transportResult_ = WeReadHttpClient::request(
      session_, url, options,
      [&](const uint8_t* data, size_t length) {
        received_ += length;
        if (received_ > 256 * 1024) return false;
        parser.feed(reinterpret_cast<const char*>(data), length);
        return !parser.hasError() && !reply_.invalid;
      },
      {}, httpStatus_);
  parser.feed(" ", 1);
  parserError_ = parser.hasError();
  if (post && !ledger_->healthy()) return Error::SdCard;
  // A verified HTTP auth rejection is decisive even if its error body is truncated.
  if (httpStatus_ == 401 || httpStatus_ == 403) return Error::SessionExpired;
  if (transportResult_ == WeReadHttpClient::Result::Aborted) return Error::Protocol;
  if (transportResult_ != WeReadHttpClient::Result::Ok) return Error::Network;
  if (!parser.hasError() && !reply_.invalid && reply_.closed &&
      (reply_.errorCode == -2012 || reply_.errorCode == -2010))
    return Error::SessionExpired;
  if (httpStatus_ != 200 || parser.hasError() || reply_.invalid || !reply_.closed || reply_.errorCode)
    return Error::Protocol;
  return Error::Ok;
}
bool Upload::resolvePosition() {
  if (strcmp(reply_.bookId, bookId_) || !reply_.hasReadingTime || (reply_.positionFields & 7) != 7) return false;
  before_ = reply_.readingTime;
  position_ = reply_.position;
  if (!(reply_.positionFields & 8)) position_.currentProgress = position_.progress;
  if (reply_.positionFields & 16) return true;
  if (position_.chapterUid == 0) {
    position_.chapterIdx = 0;
    return true;
  }
  HalFile file;
  uint32_t count = 0;
  if (!WeReadStore::openToc(WeReadStore::tocPath(bookId_), file, count)) return false;
  char uid[16];
  snprintf(uid, sizeof(uid), "%u", unsigned(position_.chapterUid));
  WeReadStore::TocRecord record;
  for (uint32_t i = 0; i < count; ++i) {
    if (!WeReadStore::readTocRecord(file, i, record)) return false;
    if (!strcmp(uid, record.chapterUid)) {
      position_.chapterIdx = record.chapterIdx;
      return true;
    }
  }
  return false;
}
Upload::~Upload() {
  session_.reset();
  // Cancellation after sending cannot put an uncertain batch back into pending.
  if (ledger_ && !ledger_->quarantineBatch()) LOG_ERR("WRNative", "Failed to persist cancelled batch");
}
bool Upload::step(Error& error) {
  error = Error::Ok;
  if (phase_ == Phase::Verify && verifyWaiting_) {
    if (static_cast<int32_t>(millis() - nextVerify_) < 0) return false;
    verifyWaiting_ = false;
  }
  char path[128];
  switch (phase_) {
    case Phase::Load:
      if (!TimeUtils::getCurrentValidTimestamp())
        error = Error::Clock;
      else if (!loadCredentials())
        error = Error::SessionExpired;
      else
        phase_ = Phase::Config;
      break;
    case Phase::Config:
      error = request("/config?token=1");
      if (error == Error::Ok) {
        if (!token(reply_.configToken))
          error = Error::Protocol;
        else {
          strcpy(token_, reply_.configToken);
          phase_ = Phase::Feature;
        }
      }
      break;
    case Phase::Feature:
      error = request("/feature?synckey=0");
      if (error == Error::Ok) {
        if (reply_.guestToken[0]) {
          if (!token(reply_.guestToken)) {
            error = Error::Protocol;
            break;
          }
          strcpy(guest_, reply_.guestToken);
        }
        phase_ = Phase::Info;
      }
      break;
    case Phase::Info:
      snprintf(path, sizeof(path), "/book/info?bookId=%s", bookId_);
      error = request(path);
      if (error == Error::Ok) {
        if (strcmp(reply_.bookId, bookId_) || !reply_.version)
          error = Error::Protocol;
        else {
          version_ = reply_.version;
          phase_ = Phase::Progress;
        }
      }
      break;
    case Phase::Progress:
      snprintf(path, sizeof(path), "/book/getProgress?bookId=%s", bookId_);
      error = request(path);
      if (error == Error::Ok) {
        if (!resolvePosition())
          error = Error::Unavailable;
        else
          phase_ = Phase::Post;
      }
      break;
    case Phase::Post: {
      // Do not risk sending the only POST on a keep-alive socket closed while
      // the device was between steps. Never retry this non-idempotent request.
      session_.reset();
      // Signing needs 3.5 KiB, too large for the task stack. Allocate only for
      // this explicit batch, after releasing TLS, and free BEFORE reconnecting.
      auto scratch = makeUniqueNoThrow<Scratch>();
      if (!scratch) {
        LOG_ERR("WRNative", "OOM: signing scratch (%u bytes)", unsigned(sizeof(Scratch)));
        error = Error::OutOfMemory;
        break;
      }
      unsigned count = ledger_->batchHours(hours_, WeReadTimeLedger::kBatchHours);
      if (!batch(credentials_, bookId_, version_, position_, hours_, count, guest_, token_,
                 TimeUtils::getCurrentValidTimestamp(), static_cast<uint32_t>(random(0, 1000)), *scratch, sha256, body_,
                 sizeof(body_))) {
        error = Error::Protocol;
        break;
      }
      scratch.reset();
      error = request("/book/batchUploadProgress", true);
      if (error == Error::Ok) {
        if (!reply_.succeeded)
          error = Error::Protocol;
        else {
          postAcknowledged_ = true;
          verifyAttempts_ = 0;
          nextVerify_ = millis() + 500;
          verifyWaiting_ = true;
          phase_ = Phase::Verify;
        }
      }
      break;
    }
    case Phase::Verify:
      ++verifyAttempts_;
      snprintf(path, sizeof(path), "/book/getProgress?bookId=%s", bookId_);
      error = request(path);
      // Only a confirmed POST reaches Verify. Re-query its result on a fresh
      // connection for transient read failures or a lagging statistic; never
      // send the duration again. Three GETs maximum while this screen is open.
      if (verifyAttempts_ < 3 &&
          (error == Error::Network ||
           (error == Error::Ok && !strcmp(reply_.bookId, bookId_) && reply_.hasReadingTime &&
            (reply_.readingTime < before_ || reply_.readingTime - before_ < ledger_->inFlightSeconds())))) {
        session_.reset();
        nextVerify_ = millis() + 1500 * verifyAttempts_;
        verifyWaiting_ = true;
        error = Error::Ok;
        return false;
      }
      if (error == Error::Ok) {
        if (strcmp(reply_.bookId, bookId_) || !reply_.hasReadingTime || reply_.readingTime < before_ ||
            reply_.readingTime - before_ < ledger_->inFlightSeconds())
          error = Error::Protocol;
        else if (!ledger_->acknowledgeBatch())
          error = Error::SdCard;
        else {
          phase_ = Phase::Done;
          session_.reset();
          return true;
        }
      }
      break;
    case Phase::Done:
      return true;
  }
  if (error != Error::Ok) {
    failurePhase_ = phase_;
    saveDiagnostic(error);  // Capture the in-flight amount before quarantining it.
    // A timeout, uncertain reply, or delayed statistic must never cause replay.
    if (!ledger_->quarantineBatch()) error = Error::SdCard;
    session_.reset();
    phase_ = Phase::Done;
    return true;
  }
  return false;
}
void Upload::diagnosticCode(char* out, size_t capacity) const {
  snprintf(out, capacity, "U%u N%u T%u:%d H%d E%d V%u", unsigned(failurePhase_), unsigned(diagnostic_.stage),
           unsigned(diagnostic_.tlsStage), diagnostic_.tlsError, httpStatus_, reply_.errorCode,
           unsigned(verifyAttempts_));
}
void Upload::saveDiagnostic(Error error) {
  // Reuse the dead POST buffer, once per terminal failure. No personal IDs,
  // tokens, request bodies or background writes; ledger files remain unchanged.
  const int n = snprintf(
      body_, sizeof(body_),
      "version=%s\nutc=%lu\nupload_phase=%u\nerror=%u\nhttp_stage=%u\nhttp_status=%d\ntransport_result=%u\n"
      "tls_stage=%u\ntls_error=%d\nfirst_tls_error=%d\nreceived=%u\napi_error=%d\njson_error=%u\ninvalid=%u\n"
      "closed=%u\npost_acknowledged=%u\nverify_attempts=%u\ncloud_before=%llu\ncloud_observed=%llu\n"
      "has_cloud_observed=%u\ninflight_seconds=%lu\nfree_before=%lu\nlargest_before=%lu\nfree_after=%lu\n"
      "largest_after=%lu\n",
      CROSSPOINT_VERSION, static_cast<unsigned long>(TimeUtils::getCurrentValidTimestamp()), unsigned(failurePhase_),
      unsigned(error), unsigned(diagnostic_.stage), httpStatus_, unsigned(transportResult_),
      unsigned(diagnostic_.tlsStage), diagnostic_.tlsError, diagnostic_.firstTlsError, unsigned(received_),
      reply_.errorCode, unsigned(parserError_), unsigned(reply_.invalid), unsigned(reply_.closed),
      unsigned(postAcknowledged_), unsigned(verifyAttempts_), static_cast<unsigned long long>(before_),
      static_cast<unsigned long long>(reply_.readingTime), unsigned(reply_.hasReadingTime),
      static_cast<unsigned long>(ledger_->inFlightSeconds()), static_cast<unsigned long>(diagnostic_.freeBefore),
      static_cast<unsigned long>(diagnostic_.largestBefore), static_cast<unsigned long>(diagnostic_.freeAfter),
      static_cast<unsigned long>(diagnostic_.largestAfter));
  if (n <= 0 || size_t(n) >= sizeof(body_) || !WeReadStore::ensureRoot()) return;
  HalFile file;
  if (Storage.openFileForWrite("WRNative", "/.crosspoint/weread/native-time-error.txt", file) &&
      file.write(reinterpret_cast<const uint8_t*>(body_), n) == size_t(n))
    file.flush();
}
static_assert(sizeof(Upload) <= 8 * 1024, "Native upload fixed workspace exceeds 8 KiB");
}  // namespace WeReadNativeTime
