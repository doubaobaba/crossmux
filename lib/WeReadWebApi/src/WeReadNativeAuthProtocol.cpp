#include "WeReadNativeAuthProtocol.h"

#include <cstdio>
#include <cstring>

namespace WeReadNativeAuth {
using namespace WeReadNativeProtocol;
namespace {
// Refresh tokens are opaque (real replies include '@') and are neither used nor saved.
// Stream them past like profile fields; do not apply access-token validation to them.
enum Field {
  Vid,
  AccessToken,
  DeviceId,
  InstallId,
  Timestamp,
  Signature,
  Uuid,
  Code,
  BookId,
  ErrCode,
  Errcode,
  QrStatus
};
constexpr const char* kFields[] = {"vid",  "accessToken", "deviceId", "installId", "timeStamp", "signature",
                                   "uuid", "wx_code",     "bookId",   "errCode",   "errcode",   "wx_errcode"};
int field(const Reply& r) {
  if (r.depth != 1) return -1;
  for (unsigned i = 0; i < sizeof(kFields) / sizeof(kFields[0]); ++i) {
    if (!strcmp(r.key, kFields[i])) return i;
  }
  return -1;
}
bool copy(char* out, size_t capacity, const char* v, size_t n) {
  if (n >= capacity || memchr(v, 0, n)) return false;
  memcpy(out, v, n);
  out[n] = 0;
  return true;
}
void onKey(void* ctx, const char* v, size_t n) {
  auto& r = *static_cast<Reply*>(ctx);
  if (!copy(r.key, sizeof(r.key), v, n)) r.key[0] = 0;
}
void scalar(void* ctx, const char* v, size_t n) {
  auto& r = *static_cast<Reply*>(ctx);
  const int i = field(r);
  if (i < 0) return;
  const uint32_t bit = 1U << i;
  if (r.seen & bit) r.invalid = true;
  r.seen |= bit;
  char* fields[] = {r.credentials.vid,
                    r.credentials.accessToken,
                    r.credentials.deviceId,
                    r.credentials.installId,
                    r.timestamp,
                    r.signature,
                    r.uuid,
                    r.code,
                    r.bookId};
  const size_t caps[] = {sizeof(r.credentials.vid),
                         sizeof(r.credentials.accessToken),
                         sizeof(r.credentials.deviceId),
                         sizeof(r.credentials.installId),
                         sizeof(r.timestamp),
                         sizeof(r.signature),
                         sizeof(r.uuid),
                         sizeof(r.code),
                         sizeof(r.bookId)};
  if (i < ErrCode) {
    if (!copy(fields[i], caps[i], v, n) || (n && !token(fields[i], i == Vid || i == Timestamp || i == BookId)))
      r.invalid = true;
    return;
  }
  bool negative = n && *v == '-';
  if (negative) {
    ++v;
    --n;
  }
  uint32_t number = 0;
  if (!n) r.invalid = true;
  for (size_t j = 0; j < n; ++j) {
    if (v[j] < '0' || v[j] > '9' || number > (INT32_MAX - 9U) / 10U) {
      r.invalid = true;
      return;
    }
    number = number * 10 + (v[j] - '0');
  }
  const int result = negative ? -static_cast<int>(number) : static_cast<int>(number);
  if (i == QrStatus) {
    r.qrStatus = result;
    r.hasQrStatus = true;
  } else if (result)
    r.errorCode = result;
}
void badScalar(void* ctx) {
  auto& r = *static_cast<Reply*>(ctx);
  if (field(r) >= 0) r.invalid = true;
}
void number(void* ctx, const char* v, size_t n) {
  auto& r = *static_cast<Reply*>(ctx);
  const int i = field(r);
  if (i >= 0 && i != Vid && i != Timestamp && i != BookId && i < ErrCode) {
    r.invalid = true;
    return;
  }
  scalar(ctx, v, n);
}
void boolean(void* ctx, bool) { badScalar(ctx); }
void objectStart(void* ctx) {
  auto& r = *static_cast<Reply*>(ctx);
  if (r.closed || field(r) >= 0) r.invalid = true;
  ++r.depth;
  r.key[0] = 0;
}
void arrayStart(void* ctx) {
  auto& r = *static_cast<Reply*>(ctx);
  if (r.depth == 0) r.invalid = true;
  objectStart(ctx);
}
void end(void* ctx) {
  auto& r = *static_cast<Reply*>(ctx);
  if (--r.depth == 0) r.closed = true;
  r.key[0] = 0;
}
void chunks(void* ctx, const char*, size_t, bool) { badScalar(ctx); }
bool fits(int n, size_t capacity) { return n >= 0 && static_cast<size_t>(n) < capacity; }
}  // namespace
JsonCallbacks Reply::callbacks() {
  return {this, onKey, scalar, number, boolean, badScalar, objectStart, end, arrayStart, end, chunks};
}
bool validCredentials(const Credentials& c, const char* expectedVid) {
  return expectedVid && token(expectedVid, true) && token(c.vid, true) && !strcmp(c.vid, expectedVid) &&
         token(c.accessToken) && token(c.deviceId) && token(c.installId);
}
bool loginBody(const Credentials& device, const char* code, uint64_t timestamp, uint32_t randomValue, Sha256 hash,
               char* out, size_t capacity) {
  if (!out || !capacity || !hash || !token(device.deviceId) || !token(device.installId) || !token(code) ||
      strlen(code) >= 256 || timestamp < 1577836800000ULL || timestamp > 4102444800000ULL || randomValue >= 1000)
    return false;
  char input[128], signature[65];
  const int size = snprintf(input, sizeof(input), "%llu%s%lu", static_cast<unsigned long long>(timestamp),
                            device.deviceId, static_cast<unsigned long>(randomValue));
  if (!fits(size, sizeof(input)) || !hash(reinterpret_cast<const uint8_t*>(input), size, signature)) return false;
  return fits(snprintf(out, capacity,
                       "{\"appFirstInstall\":1,\"code\":\"%s\",\"deviceId\":\"%s\",\"deviceName\":\"BOOX\","
                       "\"installId\":\"%s\",\"isAutoLogout\":0,\"isFromQrcode\":1,\"random\":%lu,"
                       "\"signature\":\"%s\",\"timestamp\":%llu,\"trackId\":\"\",\"deviceType\":3}",
                       code, device.deviceId, device.installId, static_cast<unsigned long>(randomValue), signature,
                       static_cast<unsigned long long>(timestamp)),
              capacity);
}
bool sessionJson(const Credentials& c, const char* expectedVid, char* out, size_t capacity) {
  if (!out || !capacity || !validCredentials(c, expectedVid)) return false;
  // Refresh tokens are not needed: expired sessions recover through explicit on-device QR login.
  return fits(
      snprintf(out, capacity, "{\"vid\":\"%s\",\"accessToken\":\"%s\",\"deviceId\":\"%s\",\"installId\":\"%s\"}\n",
               c.vid, c.accessToken, c.deviceId, c.installId),
      capacity);
}
}  // namespace WeReadNativeAuth
