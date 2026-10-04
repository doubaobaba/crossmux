#pragma once

#include <StreamingJsonParser.h>

#include "WeReadNativeProtocol.h"

namespace WeReadNativeAuth {
// Only selected top-level scalars are retained. QR bitmap/profile payloads are streamed past.
struct Reply {
  WeReadNativeProtocol::Credentials credentials;
  char timestamp[24] = {}, signature[128] = {}, uuid[128] = {}, code[256] = {}, bookId[64] = {};
  char key[32] = {};
  int errorCode = 0, qrStatus = 0, depth = 0;
  uint32_t seen = 0;
  bool invalid = false, closed = false, hasQrStatus = false;
  JsonCallbacks callbacks();
};
bool validCredentials(const WeReadNativeProtocol::Credentials& credentials, const char* expectedVid);
bool loginBody(const WeReadNativeProtocol::Credentials& device, const char* code, uint64_t timestamp,
               uint32_t randomValue, WeReadNativeProtocol::Sha256 hash, char* out, size_t capacity);
bool sessionJson(const WeReadNativeProtocol::Credentials& credentials, const char* expectedVid, char* out,
                 size_t capacity);
}  // namespace WeReadNativeAuth
