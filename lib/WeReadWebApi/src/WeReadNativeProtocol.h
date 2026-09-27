#pragma once
#include <cstddef>
#include <cstdint>

#include "WeReadTimeLedger.h"
namespace WeReadNativeProtocol {
using Sha256 = bool (*)(const uint8_t*, size_t, char[65]);
struct Credentials {
  char vid[64] = {}, accessToken[256] = {}, refreshToken[256] = {};
  char deviceId[64] = {}, installId[64] = {};
};
struct Position {
  uint32_t chapterUid = 0, chapterIdx = 0, chapterOffset = 0, progress = 0, currentProgress = 0;
};
// Fixed scratch belongs to the short-lived upload heap workspace, not the reader/task stack.
struct Scratch {
  uint8_t mapped[2048] = {};
  char payload[1536] = {};
};
bool token(const char* value, bool numeric = false);
bool sign(const char* const parts[4], Scratch& scratch, Sha256 hash, char out[65]);
bool batch(const Credentials& credentials, const char* bookId, uint32_t version, const Position& position,
           const WeReadTimeLedger::Hour* hours, unsigned count, const char* guestToken, const char* configToken,
           uint32_t now, uint32_t randomValue, Scratch& scratch, Sha256 hash, char* out, size_t capacity);
}  // namespace WeReadNativeProtocol
