# Native offline reading-time sync test build

Base `c92edca422bc2a3087e22c2d47956e562ae1006e`, SDK
`094976e1d47ad7120cf461fec5f6b737eaabf13f`; version `1.6.0-time-test2`.
X3/X4 ESP32-C3 `gh_release`; original partitions and SD updater.

## Evidence

The discarded test1 branch added Web `/web/book/read` `rt` reports with 30-second
pacing (`WeReadClient.cpp:3921-3952` before this revision). That is unsuitable for
fast offline catch-up. Test2 restores the base Web progress payload and adds an
independent native `/book/batchUploadProgress` operation.

Protocol source is the official eink 2.1.2 APK from https://ink.qq.com/ and
https://weread.qq.com/web/download_ink, SHA-256
`04d86bcdc1bbd77cab4ecc4d1c04e869d76dbabc79c60c52a707c64fc3e4b75f`.
The original ARM64 `libencrypt.so` RemapString/GenSignature functions were executed
in a local emulator. Production C++ matches 30 checked-in original-library
signature fixtures and a complete 16-hour request fixture. Public mapping/salt
constants are included; account credentials are never compiled into the image.

On 2026-09-28 a separate desktop probe submitted ONE user-confirmed 300-second
sample. `succ:1` was followed by matching 300-second increases in both book time
and fixed-month time, with chapter/offset/progress unchanged. The POST returned in
0.074 seconds. This validates that sample and protocol, not X3 execution, battery
life, every account, arbitrary long batches, cross-day attribution, or future
server behavior. The sample is journaled as credited and must not be repeated.

## Behavior and boundaries

- Only new foreground reading of recognized standard WeRead EPUBs is collected.
  Menus, child activities, sleep, chapter construction and the sync screen are
  excluded. Five minutes without manual page interaction exhausts the allowance.
  This is an activity estimate, not eye tracking; automatic page turns do not
  refresh it. Local editable history and pre-install time are never imported.
- Every 60 seconds and at cover/exit, flush the per-account/per-book hourly
  ledger. Abrupt reset may lose the unflushed interval. Reliable RTC time is
  required: unknown dates are retained as unreported time, never invented later.
- Current-book **Sync Progress** resolves positions through the existing Web
  flow, then uploads duration via the native endpoint. No proportional wait:
  at most 16 occupied hours per request, immediate subsequent batches. Hours are
  Unix timestamps, with timezone fixed to Asia/Shanghai (+08:00) in this test.
  Each book is synced separately. Capacity is 64 occupied pending hours per book;
  overflow is retained in the unreported/uncertain counter, not silently posted.
  Subsecond leftovers may move to that counter to free a full queue for new hours.
- Fetch fresh cloud position, preserve it in the duration request, and sign the
  hourly distribution. Do not use another device on the same book concurrently
  during acceptance testing: the endpoint combines position and duration and
  offers no observed atomic position comparison.
- Reserve durably before POST. Only explicit `succ:1` followed by a matching
  book-time increase acknowledges the batch. Network errors, missing statistics,
  interruption or ambiguous responses quarantine it. No automatic replay, even
  if later statistics arrive. Unattempted pending hours remain retryable. This
  deliberately allows undercount rather than duplicate time in uncertain cases.
- Success/failure results turn Wi-Fi off. Retry reconnects explicitly. Returning
  to the reader retains the existing silent restart that releases network memory.
  No new task, timer, periodic Wi-Fi wakeup or per-second SD write is introduced.

## Test login import

The test build reads `/.crosspoint/weread/native-session.json` on the SD card.
Its JSON contains `vid`, `accessToken`, `deviceId`, `installId` (optional
`refreshToken` is parsed but not used). These come from the independent native
QR login, not the Web cookie. Native vid must match the currently signed-in Web
account and ledger owner. This personal file must never be shared with a build.
A missing, mismatched or expired native session fails without posting and asks
for re-import. In-device native QR login and automatic token renewal are NOT
implemented in this test build; re-import a fresh desktop login when needed.

## TLS

The native session opts into DigiCert Global Root G2 verification with hostname
checking. Its current official chain was independently validated with OpenSSL.
The pinned `freeink-sdk` submodule includes the SecureNet fix: fail closed on
missing/bad trust roots and enable peer/domain verification. No native
request falls back to insecure TLS. A connection cannot be reused across trust
settings. Existing Web endpoints retain their original transport behavior.
A future server root change requires a firmware trust-root update.

## Resources and verification

One ~700-byte fixed ledger and clock in the reader, one ledger in the sync
activity; 684 bytes per SD slot, two slots. Commit uses two 684-byte byte buffers
and a bounded record copy, not an unbounded queue. The book-open session uses
checked heap allocation and is released immediately. Native sync additionally
uses one checked, activity-scoped allocation capped at 12 KiB: request body,
signing scratch, parser reply and credentials. This cannot live on the small
reader task stack; it is freed on completion/cancellation/reset. TLS allocations
are existing transport behavior. Measure actual free heap and power on X3.

Host tests cover hour/midnight splitting, idle gaps, invalid clock, queue bounds,
subseconds, account separation, torn writes, reservation failure, non-replay,
original-library signature fixtures, JSON field paths and numeric bounds, request
payload limits, and explicit acknowledgement plus statistic verification.
The unchanged `0x640000` app slot must fit the final image. Cross-day server
credit and physical UI/heap/power remain acceptance tests, not host-test claims.

## Device acceptance

1. Back up SD (including hidden `.crosspoint`). Import the personal native session
   separately; copy test2 `update.bin` and use the working CrossMux SD updater.
2. Confirm About shows `1.6.0-time-test2`. Connect once and verify clock/date before
   reading offline. Compare the phone's starting book/day time.
3. Read a NEW short session, with normal manual page turns. Open Sync Progress,
   resolve direction if asked, and wait for the short network sequence.
4. Check submitted/pending/uncertain counters and refresh the phone's statistics.
   Repeat sync without more reading: previously acknowledged time must not recur.
5. Verify sleep/resume, normal reading/fonts, return to reader, Wi-Fi shutdown,
   heap stability and battery. Do not deliberately interrupt an SD write.
6. Separately test across an hour and midnight, documenting server attribution.

Rollback uses the preserved working CrossMux 1.6.0 `update.bin` in the same SD
menu. Test2 does not migrate normal progress/books/Web sessions/local statistics.
The withdrawn test1 v1 outbox is deliberately not migrated because it contains
no reliable hourly dates.

## Fork checkout and build

The tested firmware sources are on `codex/weread-time-sync`, based on the commit
at the top of this document. The fork's `main` branch is not this test build.
The SDK fix is pinned to a commit in `doubaobaba/freeink-sdk`; no separate patch
application or SDK branch switch is required.

```bash
git clone --recurse-submodules --branch codex/weread-time-sync https://github.com/doubaobaba/crossmux.git
cd crossmux
```

For an existing checkout of that branch, use `git pull --ff-only`, then
`git submodule sync --recursive` and `git submodule update --init --recursive`.
Do not use `git submodule update --remote`: it bypasses the tested SDK pin.

Build in an isolated Python environment with `pioarduino==6.1.19` (the version
used by this repository's CI), then run `pio run -e gh_release`. The tested local
toolchain used CMake 3.31.10, Arduino ESP32 3.3.7, ESP-IDF 5.5.2.260206 and
RISC-V GCC 14.2.0+20251107. On macOS the PlatformIO `tool-cmake` package needed
a local override to CMake 3.31.10. Machine-specific overrides and build products
are intentionally untracked; see [build-system.md](build-system.md).

For SD installation, copy `.pio/build/gh_release/firmware.bin` as `update.bin`.
The previously compiled test image is 6,210,704 bytes (OTA slot: 6,553,600 bytes),
SHA-256 `cf1adb529afe2e5376b33574713354a422434aed2d8cdfdde8b0d3abacd151da`.
Build timestamps/toolchains can change binary hashes; this identifies the
existing test artifact, not a reproducible-build guarantee.

Validation completed before publishing: 96 targeted CTest cases (including
three native protocol test groups), four reading UI regression cases, signature
fixtures, image checksum, OTA size, and patch reconstruction. The broader suite
passed 567/568; its macOS compiler-header-path failure was checked separately
with `/usr/bin/c++` for both variants. Physical X3, battery and cross-day checks
remain the device acceptance steps above.

### 中文测试提示

拉取时务必选择 `codex/weread-time-sync`，并带上子模块。版本应显示
`1.6.0-time-test2`。卡上的私人 `native-session.json` 需单独保留；仓库不包含
账号凭据，固件也不会自动生成这份原生登录文件。

先正常翻页阅读新的 2–3 分钟，再按原来的“同步进度”。先处理进度方向，随后
自动同步当前书的待传时长。每批最多包含 16 个有阅读记录的小时，不需要等待
相同长度的阅读时间。核对手机统计后，不再阅读并重复同步，确认不会重复计时。
