# Native offline reading-time sync test build

Release status (2026-10-08): the owner accepted the current X3 build for release
as `weread-time-v1`. The test8 binary is promoted unchanged, including its
embedded version string. See [release notes and known limitations](../releases/weread-time-v1.md).
The entries below retain their original test-stage observations; they are not
a claim that every acceptance scenario subsequently passed.

Test8 base `1fcdc79be5e473f575722f9219501fc7caf465f7`; version `1.6.0-time-test8`.
The SDK is pinned by the `freeink-sdk` gitlink; the output manifest records its exact commit.
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

## On-device native login (test3)

The current-book **Sync Progress** screen validates a saved native session before
starting Web progress sync. It reads `/.crosspoint/weread/native-session.json`;
test2 imports remain compatible. The file is separate from the firmware image.
With a missing, corrupt, mismatched or expired native session, X3 displays a
WeChat QR code in this same screen. Scan with the account used by the existing
WeRead shelf, confirm on the phone, and sync resumes automatically after saving.
Ordinary Web shelf login remains separate: its cookies cannot be substituted by
a native access token. A missing/expired Web session still uses Apps > WeRead.

The device fetches `/wxticket`, requests the official SDK QR endpoint, polls
`long.open.weixin.qq.com` while the QR screen is visible, and exchanges the code
at `/login`. These are the same requests as the previously verified desktop
probe. Native `vid` must equal the current shelf/ledger owner before saving.
A different account is rejected without overwriting the previous session or
submitting reading data. The saved file contains only `vid`, `accessToken`,
`deviceId`, `installId`; refresh tokens are not persisted or used.

QR lifetime is bounded to five minutes. Poll reads time out after six seconds
and wait at least 1.5 seconds between requests. Returning cancels at the next
request boundary; no background worker continues polling. QR expiry, declined
login and network/storage failure offer Back/Retry. Login failure is terminal
for that authorization code. New login requires a new attempt, not a hidden
retry of the exchange. A native expiry during time upload gets at most one
automatic re-login per sync visit. Existing uncertain batches remain quarantined.

Saving writes and flushes a `.part` file, verifies its exact bytes, then uses the
existing backup-and-rename replacement. If interrupted between renames, loading
recovers the previous `.bak` session. Read-back must succeed before sync resumes.
This is recovery against common interrupted-write cases, not a guarantee against
arbitrary FAT/SD corruption. Sessions, backups and temporary copies are ignored
by Git and must never be published. Already-saved reading ledgers are unchanged.

The test3 native login workspace, including its streaming parser, is capped at
8 KiB and allocated with checked nothrow allocation only during manual sync.
It is freed before Web sync or the separate native upload workspace is created.
Existing verified sessions add one read-only native progress check; they do not
trigger QR login or write session data. There are no periodic background renewals.

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
uses one checked, activity-scoped allocation capped at 8 KiB: request body,
parser reply and credentials. The separate 3.5 KiB signing scratch is released
before TLS. The inactive Web workspace is freed before native upload. None of
these large objects live on the reader task stack. TLS allocations
are existing transport behavior. Measure actual free heap and power on X3.

Host tests cover hour/midnight splitting, idle gaps, invalid clock, queue bounds,
subseconds, account separation, torn writes, reservation failure, non-replay,
original-library signature fixtures, JSON field paths and numeric bounds, request
payload limits, and explicit acknowledgement plus statistic verification.
The unchanged `0x640000` app slot must fit the final image. Cross-day server
credit and physical UI/heap/power remain acceptance tests, not host-test claims.

## Device acceptance

1. Back up SD (including hidden `.crosspoint`). Copy test8 `update.bin` and use
   the working CrossMux SD updater. Preserve existing books, sessions and ledgers.
2. Confirm About shows `1.6.0-time-test8`. Connect once and verify clock/date before
   reading offline. Compare the phone's starting book/day time.
3. Read a NEW short session, with normal manual page turns. Open Sync Progress,
   scan/confirm if prompted, resolve direction if asked, and wait for the network
   sequence. Repeat with a valid saved session: no QR should appear.
4. Check submitted/pending/uncertain counters and refresh the phone's statistics.
   Repeat sync without more reading: previously acknowledged time must not recur.
5. Verify sleep/resume, normal reading/fonts, return to reader, Wi-Fi shutdown,
   heap stability and battery. Do not deliberately interrupt an SD write.
6. Separately test across an hour and midnight, documenting server attribution.

Rollback uses the preserved test2 or working CrossMux 1.6.0 `update.bin` in the
same SD menu. Test3 does not migrate normal progress/books/Web sessions/local
statistics or the WRTM v2 hourly ledger; test2 pending time remains pending.
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
The previously compiled test2 image is 6,210,704 bytes (OTA slot: 6,553,600 bytes),
SHA-256 `cf1adb529afe2e5376b33574713354a422434aed2d8cdfdde8b0d3abacd151da`.
Build timestamps/toolchains can change binary hashes; this identifies the
previous test2 artifact, not test3 or a reproducible-build guarantee. Test3 size
and checksum are recorded in its separate build manifest.

Validation completed before publishing: 96 targeted CTest cases (including
three native protocol test groups), four reading UI regression cases, signature
fixtures, image checksum, OTA size, and patch reconstruction. The broader suite
passed 567/568; its macOS compiler-header-path failure was checked separately
with `/usr/bin/c++` for both variants. The owner subsequently reported successful test2 time synchronization on X3.
Test3 passes 98 targeted CTest entries (including three login test groups and
four reading UI checks). It adds host fault-injection coverage for the actual login state machine,
protocol parsing/signing, session replacement and Activity resume/recovery.
Test3 QR operation, physical heap/battery and cross-day attribution still need
device acceptance; no new duration was submitted during development.

### 中文测试提示

拉取时务必选择 `codex/weread-time-sync`，并带上子模块。版本应显示
`1.6.0-time-test5`。凭证一直是独立保存在 SD 卡里的，不在固件或仓库中。
第一次同步或凭证过期时，会在 X3 上显示二维码；使用当前书架对应的微信账号
扫码并确认，设备保存后自动继续。有效凭证会直接复用，不需要每次扫码。

升级保留现有书籍、进度和 WRTM v2 待同步记录。按每本书原来的“同步进度”分别
补传；每批最多包含 16 个有阅读记录的小时，不需要等待同等长度的阅读时间。
未知日期、结果不确定的旧批次仍不会自动重传。云端能否接收较早日期以及按天
如何入账，以手机端核对为准；保留卡上记录不等于保证云端接受全部历史时长。

首次实测核对：过期凭证出现二维码；确认后继续同步；重启后再次同步无需扫码；
不继续阅读而重复同步不会重复计时。二维码页返回、过期后重试以及扫错账号都应
保留待同步时长。不要为了测试而清空 `.crosspoint` 或删除时长目录。


## Test4: pre-QR failure diagnosis

The owner reported a test3 network error before seeing a QR code, while normal
Web shelf networking still worked. On 2026-10-04, desktop read-only probes using
the embedded root returned HTTP 401 / `errcode: -2012` for the expired native
session, HTTP 200 for `/wxticket`, and HTTP 200 with a 68,114-byte SDK QR response.
All three real responses pass the production streaming auth parser. This does
not identify the X3 failure: device transport, clock and heap need device evidence.
No reading-time POST was sent in this investigation.

`Login::request` and `Upload::request` previously returned Network before looking
at a verified HTTP 401/403 when response reading aborted or broke. Test4 treats
that auth rejection as SessionExpired even with an incomplete error body. A
callback abort caused by invalid/oversized JSON is Protocol, not a network timeout;
API error fields are trusted only in complete, valid responses. The saved session
and unattempted ledgers remain intact, and uncertain uploads are never replayed.
This is a confirmed error-classification defect, not a confirmed explanation of
the owner's device failure.

Native login failures now display the operation and a compact numeric code. The
last terminal failure also overwrites `/.crosspoint/weread/native-auth-error.txt`.
It contains only version, UTC, numeric stages/errors, response byte count, parser
flags, and before/after heap totals/largest blocks. It does not contain URLs,
headers, credentials, QR codes, account IDs, book IDs, or response bodies. It reuses
the existing request buffer, writes at most 1 KiB once per failed attempt, and adds
no worker, timer, background network or polling writes. Successful QR waits do not
write diagnostics. A diagnostic write failure does not change the original error.
The existing <8 KiB login and <12 KiB upload workspace limits still compile.

Code fields:

- `A`: auth phase: 0 load, 1 check saved session, 2 ticket, 3 QR, 4 poll, 5 exchange, 6 save.
- `N`: HTTP stage: 1 Wi-Fi, 2 setup, 3 connect, 4 write, 5 status, 6 headers, 7 body, 8 complete.
- `Tstage:error`: SDK stage: 1 TCP, 2 context allocation, 3 trust root, 4 TLS session allocation,
  5 hostname setup, 6 handshake, 7 handshake timeout, 8 write, 9 read. The signed error is the
  original wolfSSL result; zero means no numeric TLS error was supplied. SD diagnostics also
  retain the initial handshake error if the existing TLS 1.2 fallback was attempted.
- `H`: HTTP status (`-1` means none received); `E`: parsed API error (zero alone is not success).

Test4 host verification adds actual HTTP framing/failure-stage execution, truncated
401/403 handling, the observed lowercase error field, a 68 KiB ignored QR bitmap,
protocol-vs-network classification, terminal diagnostic privacy/storage behavior,
and SDK error retention after cleanup. These use deterministic transport faults;
physical TLS/memory, on-screen layout, QR scan and battery remain unverified.

To collect evidence without a USB cable: update from SD, open the same book's
Sync Progress once, and photograph any failure screen. For more detail, power off,
put the SD card in the computer, and read only `native-auth-error.txt`. Preserve
`.crosspoint`, native sessions and time ledgers. Retrying after a failed login is
safe for unattempted pending time; already-uncertain batches remain quarantined.


## Test5: cross-signed QR certificate chain

The owner's test4 screen reported `A3 N3 T6:-188 H-1 E0`. The SD diagnostic
confirmed auth phase 3 (QR), HTTP connect stage 3, initial and fallback handshake
error -188, and zero response bytes. Arduino-wolfSSL 5.7.2's
`wolfssl/wolfcrypt/error-crypt.h:141` defines -188 as `ASN_NO_SIGNER_E`.
This failure occurs before login confirmation or any reading-time upload.

Live certificate capture on 2026-10-04 showed the difference:

- `i.weread.qq.com`: leaf, DigiCert Secure Site OV G2 intermediate.
- `open.weixin.qq.com` and `long.open.weixin.qq.com`: leaf, same intermediate,
  plus DigiCert Global Root G2 cross-signed by DigiCert Global Root CA.

The firmware already trusts self-signed DigiCert Global Root G2. Its existing
wolfSSL build nevertheless required every presented CA to verify, including the
extra cross-signed certificate whose older issuer is not in the trust store.
This is documented in wolfSSL `src/internal.c:35-39,14895-14918` and the
[official alternate-chain explanation](https://www.wolfssl.com/configuring-wolfssl-alternate-certificate-chain-feature-enabled/).

Test5 adds `WOLFSSL_ALT_CERT_CHAINS` to the shared build flags. The peer still must
validate through the existing trusted G2 root, with signatures, validity dates
and hostname checked. It does not add an older trust root, accept arbitrary peer
certificates, disable TLS verification, or introduce a verify callback. The SDK
pin, all login/time-sync logic, ledger format and SD update method are unchanged.
No new application allocation, worker, timer, background network or card-write
path is introduced; physical peak heap and power remain device acceptance checks.

For verification, the exact installed Arduino-wolfSSL 5.7.2 C sources and patched
`user_settings.h` were compiled on the host with the firmware wolfSSL flags in
two variants differing only in `WOLFSSL_ALT_CERT_CHAINS`. POSIX sockets replaced
the ESP32 TCP transport; this is not a claim of MCU heap or timing equivalence.
Both variants loaded the unchanged firmware G2 root and requested X25519 and
2 KiB TLS fragments, matching SecureClient.

All 24 handshake cases matched their expected result:

- Before: both WeChat QR hosts fail -188 in TLS 1.3 and TLS 1.2; native WeRead
  succeeds in both versions (6 cases).
- After: all three hosts succeed in both versions (6 cases).
- After with an incorrect expected hostname: all three hosts reject with -322
  in both versions (6 cases).
- After with an unrelated trusted root: all three hosts still reject with -188
  in both versions (6 cases).

The rebuilt wolfSSL then performed read-only `/wxticket` and SDK QR GET requests:
both returned HTTP 200, the QR response had `errcode:0` and a 16-character UUID.
No QR authorization was confirmed, login code exchanged, or reading duration
submitted from the computer. The test5 package includes a sanitized result matrix
and test logs. The 98 targeted CTest entries also pass. The next physical check is
to install test5, obtain the QR screen, confirm the same account, and verify time
sync plus a second sync with the saved session.

### 本次修复

你的 test4 日志已经定位到二维码接口的证书链校验失败。同版本加密库已复现
相同的 `-188`；启用替代证书链支持后，二维码获取成功，错误域名和不可信根证书
仍被拒绝。test5 只修正这个兼容问题，不改阅读记录、登录保存或上传规则。
升级后版本应为 `1.6.0-time-test5`，继续在原来的书里点“同步进度”验证扫码。


## Test6: accept opaque, unused refresh tokens after QR confirmation

The owner confirmed that test5 displays the QR code on X3. After confirmation,
the device showed `A5 N7 T0:0 H200 E0`. Its SD diagnostic captured exchange phase
5, HTTP 200, transport Aborted, 592 response bytes, JSON error 0, invalid 1,
closed 1, and no TLS error. The login parser rejected a field in complete JSON;
this failure occurs before Web progress or reading-time upload.

The private credential saved by the successful desktop login contains `@` in
`refreshToken`. Feeding those real saved login fields through the production
test5 parser reproduces invalid=1 while all required credentials validate.
Removing only the unused refresh token makes the same input pass. No private
values are in fixtures, logs, firmware, or this repository.

Test6 omits `refreshToken` from the selected-field table and streams past it,
just like other unused response metadata. It is not consumed by sync, retained
by the login reply parser, or persisted. Required fields keep their existing
bounds, type/duplicate checks, safe-character rules and account binding; this
does not broaden access-token or signature validation. Named field indices
replace the numeric checks affected by removing that field. There is no new
allocation, network request, task, timer or SD write, and no ledger format change.

The real private credential now passes the production parser. A synthetic
regression uses an opaque `@` token in the full login/save lifecycle; parser
coverage includes before/after required fields, 1/7/512/1024-byte chunks, long
optional values, null/nested optional values, malformed JSON and invalid or
duplicate required credentials. Existing lifecycle tests still cover account
mismatch, interrupted/failed saves, session reuse and expiry. The full current
login response was not captured and no new desktop login or time upload was
performed: final physical login/save/time-sync acceptance remains an X3 check.

升级到 `1.6.0-time-test6` 后，在原来的书中点“同步进度”，扫码并确认。
成功后再点一次同步，应复用已保存凭证；手机端核对时长即可。卡上历史记录
保持原样，本次电脑端不提交任何阅读时长。


## Test7: bounded result confirmation and upload diagnostics

The owner confirmed that test6 completes QR login and saves the native session.
A subsequent time upload stopped with a Network error and quarantined duration;
a later attempt showed a Protocol error. The old build persisted only login
failure diagnostics, so its photos and ledger cannot establish whether the first
failure was in the POST response or in the following verification GET. Read-only
cloud statistics show matching daily buckets, but without a captured pre-upload
baseline that is not proof of full per-batch acknowledgement. Existing uncertain
records must not be reset, replayed or marked accepted from this observation.

Test7 closes two robustness/observability gaps; it is not a claim that the exact
first device transport failure was reproduced:

- Close the preparatory keep-alive session before the sole non-idempotent POST,
  avoiding reuse of a peer-closed connection between activity steps. This adds
  at most one TLS connection per attempted batch; no POST retry is introduced.
- After an explicit successful POST reply, wait 500 ms and query the book time.
  A Network failure or well-formed same-book statistic that has not reached the
  expected increment schedules another read, at most three verification GETs
  with 1.5/3-second gaps. Retry connections are fresh. Waits are nonblocking and
  work across millis rollover. Cancellation still exits at request boundaries.
  Mismatched books, malformed replies, auth rejection and SD failure stay terminal.
- Each terminal upload failure captures HTTP/TLS stage and status, parser flags,
  POST acknowledgement, verification attempt count, pre-upload/observed cloud
  seconds, and in-flight seconds before quarantine. The screen shows
  `U<phase> N<http-stage> T<tls-stage>:<tls-error> H<status> E<api-error> V<reads>`.
  Upload phases are Load0, Config1, Feature2, Info3, Progress4, Post5, Verify6.
  The SD report is `/.crosspoint/weread/native-time-error.txt`; it contains no
  credentials, account/book IDs, request body or URL. A later failure replaces it.

The failure report reuses the no-longer-needed 4096-byte POST buffer, with one
small write on terminal failure. Fixed diagnostics and counters add no separate
allocation; the existing upload workspace remains capped at 12 KiB. The operation reuses its
inactive Web-login UID scratch buffer; the activity retains a 96-byte error code. There is no new background
worker, periodic Wi-Fi activation or reading-time/card-format change. Normal
successful confirmation adds only its 500 ms delay; failure GETs remain bounded
by the existing 15-second timeout. Device peak heap and power are not host-test
claims.

Production-method regression tests exercise delayed statistics, transient GET
failure, attempt exhaustion, no POST replay, error snapshot before quarantine,
wrong-book/auth/protocol failures, timer rollover and secret-free diagnostic SD
writes. Activity coverage checks that the error code survives operation cleanup.
All tests use synthetic values; the desktop inspection used GET only and did not
submit any new or historical duration.

Upgrade without replacing session or ledger files. Confirm `1.6.0-time-test7`,
then sync a short genuinely new reading interval or an unattempted pending batch.
Existing uncertain totals will remain displayed. If failure recurs, preserve the
full U/N/T/H/E/V code and SD report before additional attempts. Do not restore an
old pending ledger over the current one: the cloud may already have accepted it.


## Test8: release idle workspaces before native TLS; reserve only before sending

The actual test7 SD diagnostic reports `U5 N3 T6:-155 H-1 E0 V0`, first TLS
error -155, zero received bytes, no HTTP status, no POST acknowledgement, and
8 in-flight seconds. Free heap before the failing request was 34,116 bytes;
the largest free block was 21,492 bytes. Both wolfSSL attempts failed before
HTTP writes. This attempt provides no evidence of an API duration restriction.

The exact Arduino-wolfSSL 5.7.2 sources reproduce -155 under a constrained host
allocator: at a 24,000-byte allocation budget, automatic TLS fails with -125
and explicit TLS 1.2 fails with -155 after a denied allocation. Both pass at
32,000 bytes. Certificate verification masks some crypto failures as -155.
These host figures are not MCU heap requirements; they establish that -155
can result from memory exhaustion, not that every -155 is an allocation failure.
The X3 cause still requires physical validation of this reduced-memory build.

Changes and resource budget:

- The progress Activity owns Web Operation, native Login, and native Upload
  as mutually exclusive checked allocations. It frees Operation before native
  TLS and frees native workspaces before Web TLS. These ~8 KiB objects cannot
  fit the task stack; allocation is limited to user-initiated stages, not pages.
- Upload no longer retains the 3,584-byte signing Scratch. A checked temporary
  exists only after releasing the previous TLS session; it is freed before
  reconnecting. Upload now has an 8 KiB static assertion (previously 12 KiB).
- ESP32-C3 debug type sizes: Operation 8,176 bytes, new Upload 8,016 bytes,
  new sync Activity 1,656 bytes. Removing the embedded Operation (replaced with
  two 4-byte pointers) and retained Scratch reduces native-stage workspace by
  11,752 bytes, before allocator metadata. Actual free heap depends on runtime
  fragmentation and other components; no TLS checks or root CAs were weakened.
- RequestOptions has an optional non-allocating beforeSend hook. The wolfSSL
  backend calls it exactly once after connection succeeds, before any HTTP
  byte. Native upload durably reserves the ledger there. DNS/TCP/TLS failure
  leaves time pending, while a write/response/confirmation failure quarantines
  the batch. The ESP HTTP backend conservatively calls the hook before open(),
  because that API writes headers while opening the connection.
- Cancellation frees TLS and quarantines any in-flight batch. The Activity's
  ledger is declared before Upload so it outlives Upload's destructor.
- No background networking, auto-replay, fabricated duration, ledger format
  change, or additional waiting service is introduced.

Host regression checks exercise actual HTTP hook order (connect failure,
callback failure, successful send, partial write, offline), production native
request reservation, OOM, mutually exclusive Activity lifetimes, auth expiry,
cancellation before/after sending, bounded GET confirmation, and no POST replay.

For this user's recovery only, the two verified SD slots retain the precise
8-second flight at sequence 102 and its quarantine at sequence 103. The
matching diagnostic proves it was never sent. Restore only those 8 seconds
into their original hourly bucket using a new checksummed sequence 104, keeping
the earlier 4,107 uncertain seconds untouched. Do not restore an old ledger or
infer the outcome of the earlier batch from this newer TLS failure. The desktop
only queries cloud statistics; it submits no reading time.

Device acceptance: upgrade to test8, read briefly, then use Sync Progress.
The restored 8 seconds plus new reading should upload and confirm promptly.
A pre-send TLS failure must leave those seconds pending rather than increasing
uncertain seconds. Preserve any new native-time-error.txt for diagnosis.
The older 4,107 seconds remain uncertain and must not be automatically replayed.
