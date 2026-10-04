"""Compile production login code with deterministic HTTP/SD faults; no real credentials/network."""
import hashlib
import json
import re
import unittest

from test_weread_time_sync import ROOT, LIB, compile_run
from test_reading_ui_regressions import method


class WeReadNativeAuthTest(unittest.TestCase):
    def test_sync_auth_gate_resume_and_expiry_recovery(self):
        source = (ROOT/'src/activities/apps/weread/webapi/WeReadProgressSyncActivity.cpp').read_text()
        methods = '\n'.join(method(source, 'void WeReadProgressSyncActivity::'+name+'(')
                            for name in ('startSync', 'startNativeLogin', 'advanceNativeLogin', 'advanceSync'))
        compile_run(r'''
#include <atomic>
#include <cassert>
#include <cstring>
#include <memory>
#include <utility>
#define LOG_ERR(...)
#define LOG_INF(...)
namespace WeReadProtocol { struct RemoteProgress { float percent=0; }; }
namespace WeReadClient {
 enum class Error { Ok, Clock, OutOfMemory, Protocol, Network, SessionExpired };
 enum class ProgressSyncMode { Compare, UploadLocal };
 enum class ProgressSyncOutcome { Pending, SelectionRequired, ApplyRemote, AlreadySynced };
 struct Operation {
  enum class Event { None, Authenticated, DetailReady, ChapterComplete, QrReady, Cancelled, Failed, Complete };
  struct Result { ProgressSyncOutcome outcome=ProgressSyncOutcome::AlreadySynced;WeReadProtocol::RemoteProgress remote; };
  Event event=Event::None; Error err=Error::Ok;unsigned starts=0,resets=0;
  bool beginProgressSync(const char*,int,ProgressSyncMode) { ++starts;return true; }
  Event step() { return event; } Error error() const { return err; }
  void reset() { ++resets; } Result progressSyncResult() { return {}; }
 };
}
using E=WeReadClient::Error;
bool allocationFails=false,clockValid=true;
unsigned authLive=0;
namespace WeReadNativeAuth {
 struct Login {
  enum class Event { None, QrReady, Scanned, Complete, Failed };
  enum class Failure { None, AccountMismatch };
  Event event=Event::None; bool ready=true,forced=false; E err=E::Ok;
  Login() { ++authLive; } ~Login() { --authLive; }
  bool begin(const char* account,const char* book,bool force) { assert(!strcmp(account,"123")&&!strcmp(book,"456"));forced=force;return true; }
  bool readyToStep() { return ready; } Event step(E& e) { e=err;return event; }
  const char* qrUrl() { return "https://open.weixin.qq.com/connect/confirm?uuid=test"; }
  Failure failure() { return Failure::AccountMismatch; }
 };
}
template<class T> std::unique_ptr<T> makeUniqueNoThrow() { if(allocationFails)return nullptr;return std::make_unique<T>(); }
namespace TimeUtils { bool isClockValid() { return clockValid; } }
struct { bool syncNow() { return false; } } halClock;
struct Cache { void clearCache() {} };
struct Renderer { Cache* getFontCacheManager() { return nullptr; } };
namespace NetworkStartup { void prepare(Renderer&) {} }
struct RenderLock { template<class T> RenderLock(T&) {} };
struct WeReadProgressSyncActivity {
 enum class State { Starting,Failed,LoginRequired,Authenticating,AuthQr,AuthScanned,AuthFailed,Syncing,
                    TimeSyncing,TimeFailed,TimeStarting,Success,ChoosingDirection };
 enum class DirectionOption { ApplyRemote,UploadLocal };
 State state_=State::Starting; Renderer renderer;
 WeReadClient::Operation operation_;
 struct { const char* account() { return "123"; } unsigned pending=300,uncertain=0;unsigned pendingSeconds() { return pending; } } timeLedger_;
 std::unique_ptr<WeReadNativeAuth::Login> nativeLogin_;
 WeReadNativeAuth::Login::Failure authFailure_=WeReadNativeAuth::Login::Failure::None;
 bool radioStopped_=false,timeReady_=true,nativeReady_=false,forceNativeLogin_=false,nativeRecoveryAttempted_=false;
 bool wifiChild=false,returned=false,uploadConflict_=false;float remoteFraction_=0;
 std::atomic<bool> fullRefreshPending_{false};
 char nativeQrUrl_[256]={}; const char* bookId_="456";int input_=0;
 E error_=E::Ok;
 WeReadClient::ProgressSyncMode syncMode_=WeReadClient::ProgressSyncMode::Compare;
 WeReadClient::ProgressSyncOutcome outcome_=WeReadClient::ProgressSyncOutcome::Pending;
 DirectionOption selectedDirection_=DirectionOption::ApplyRemote;
 void requestUpdate() {} void requestUpdateAndWait() {}
 void launchWifiSelection() { wifiChild=true; }
 void returnToReader() { returned=true; }
 void applyRemoteProgress(const WeReadProtocol::RemoteProgress&) { state_=State::Success; }
 void startSync();void startNativeLogin();void advanceNativeLogin();void advanceSync();
};
''' + methods + r'''
int main() {
 using S=WeReadProgressSyncActivity::State;
 using A=WeReadNativeAuth::Login;
 using O=WeReadClient::Operation;
 { WeReadProgressSyncActivity p;p.startSync();assert(p.state_==S::Authenticating&&authLive==1&&!p.operation_.starts);
   p.nativeLogin_->event=A::Event::QrReady;p.advanceNativeLogin();assert(p.state_==S::AuthQr&&p.nativeQrUrl_[0]);
   p.nativeLogin_->event=A::Event::Scanned;p.advanceNativeLogin();assert(p.state_==S::AuthScanned);
   p.nativeLogin_->event=A::Event::Complete;p.advanceNativeLogin();assert(p.state_==S::Starting&&p.nativeReady_&&!authLive);
   p.startSync();assert(p.state_==S::Syncing&&p.operation_.starts==1&&!authLive);
   p.operation_.event=O::Event::Complete;p.advanceSync();assert(p.state_==S::TimeStarting);
 }
 { WeReadProgressSyncActivity p;allocationFails=true;p.startSync();assert(p.state_==S::AuthFailed&&p.error_==E::OutOfMemory&&!authLive);allocationFails=false; }
 { WeReadProgressSyncActivity p;p.startSync();p.nativeLogin_->event=A::Event::Failed;p.nativeLogin_->err=E::Protocol;
   p.advanceNativeLogin();assert(p.state_==S::AuthFailed&&!authLive&&!p.nativeReady_&&!p.operation_.starts); }
 { WeReadProgressSyncActivity p;p.startSync();p.nativeLogin_->ready=false;
   p.advanceNativeLogin();assert(p.state_==S::Authenticating); } // Cancellation releases workspace.
 assert(!authLive);
 { WeReadProgressSyncActivity p;p.radioStopped_=true;p.startSync();assert(p.wifiChild&&!authLive&&!p.operation_.starts); }
 { WeReadProgressSyncActivity p;clockValid=false;p.startSync();assert(p.state_==S::Failed&&!authLive);clockValid=true; }
 // Mid-upload expiry gets exactly one automatic login; uncertain duration is not made pending again.
 { WeReadProgressSyncActivity p;p.state_=S::TimeSyncing;p.nativeReady_=true;
   p.timeLedger_.pending=0;p.timeLedger_.uncertain=300;p.operation_.event=O::Event::Failed;p.operation_.err=E::SessionExpired;
   p.advanceSync();assert(p.state_==S::Starting&&p.forceNativeLogin_&&p.nativeRecoveryAttempted_&&!p.nativeReady_);
   assert(p.timeLedger_.pending==0&&p.timeLedger_.uncertain==300);
   p.startSync();assert(p.nativeLogin_->forced);
   p.nativeLogin_->event=A::Event::Complete;p.advanceNativeLogin();assert(!authLive);
   p.state_=S::TimeSyncing;p.advanceSync();assert(p.state_==S::TimeFailed&&!authLive);
 }
 // Web-session expiry still uses its own login path; native QR cannot replace Web cookies.
 { WeReadProgressSyncActivity p;p.state_=S::Syncing;p.operation_.event=O::Event::Failed;p.operation_.err=E::SessionExpired;
   p.advanceSync();assert(p.state_==S::LoginRequired&&!p.nativeRecoveryAttempted_); }
}
''')

    def test_protocol_bounds_account_binding_and_login_signature(self):
        result = compile_run(r'''
#include "WeReadNativeAuthProtocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
using namespace WeReadNativeAuth;
bool hash(const uint8_t* data,size_t n,char out[65]) {
 uint8_t d[32];
#ifdef __APPLE__
 CC_SHA256(data,n,d);
#else
 SHA256(data,n,d);
#endif
 for(unsigned i=0;i<32;++i) snprintf(out+i*2,3,"%02x",d[i]); return true;
}
bool parse(Reply& r,const std::string& s) {
 StreamingJsonParser p(r.callbacks());
 for(char c:s) p.feed(&c,1);
 p.feed(" ",1); return !p.hasError()&&!r.invalid&&r.closed;
}
int main() {
 Reply r;
 assert(parse(r,R"({"vid":123,"accessToken":"test-token","deviceId":"test-device","installId":"test-install"})"));
 assert(validCredentials(r.credentials,"123")); assert(!validCredentials(r.credentials,"999"));
 char out[1024]; assert(sessionJson(r.credentials,"123",out,sizeof(out)));
 assert(!strstr(out,"refreshToken")); assert(!sessionJson(r.credentials,"999",out,sizeof(out)));
 assert(!sessionJson(r.credentials,"123",out,20));
 assert(loginBody(r.credentials,"test-code",1700056800123ULL,20,hash,out,sizeof(out))); puts(out);
 assert(!loginBody(r.credentials,"bad\"code",1700056800123ULL,20,hash,out,sizeof(out)));
 assert(!loginBody(r.credentials,"test",1,20,hash,out,sizeof(out)));
 r={}; assert(parse(r,R"({"timeStamp":"1700056800","signature":"abc","qrcode":{"qrcodebase64":"ignored"},"uuid":"test-uuid"})"));
 assert(!strcmp(r.timestamp,"1700056800")&&!strcmp(r.uuid,"test-uuid"));
 r={}; assert(parse(r,R"({"wx_errcode":405,"wx_code":"test-code"})")); assert(r.hasQrStatus&&r.qrStatus==405);
 r={}; assert(parse(r,R"({"errCode":-2012})")); assert(r.errorCode==-2012);
 for(const char* bad:{R"({"vid":true})",R"({"vid":null})",R"({"vid":["123"]})",
                      R"({"vid":{"vid":"123"}})",R"({"vid":"123","vid":"456"})",
                      R"([{"vid":"123"}])",R"({"wx_errcode":405.1})",R"({"wx_errcode":999999999999})",
                      R"({"vid":"123"}) {}",R"({"vid":"123" )",R"({"vid":"12\u00003"})"}) {
  r={}; if(parse(r,bad)) { fprintf(stderr,"Unexpected accepted JSON: %s\n",bad); assert(false); }
 }
 r={}; assert(!parse(r,"{\"accessToken\":\""+std::string(600,'a')+"\"}"));
 r={}; assert(parse(r,"{\"qrcode\":{\"qrcodebase64\":\""+std::string(4096,'a')+"\"},\"uuid\":\"ok\"}"));
 r={}; assert(parse(r,R"({"nested":{"vid":"123","wx_errcode":405}})"));
 assert(!r.credentials.vid[0]&&!r.hasQrStatus);
}
''', [LIB/'WeReadNativeAuthProtocol.cpp', LIB/'WeReadNativeProtocol.cpp',
      ROOT/'lib/JsonParser/StreamingJsonParser.cpp'], crypto=True)
        body = json.loads(result)
        self.assertEqual(body['signature'], hashlib.sha256(b'1700056800123test-device20').hexdigest())
        self.assertEqual(body['code'], 'test-code')
        self.assertEqual(body['deviceType'], 3)

    def test_login_lifecycle_and_storage_faults(self):
        header = (LIB/'WeReadNativeAuth.h').read_text()
        header = re.sub(r'^#(?:include.*|pragma once)\n', '', header, flags=re.M)
        source = (LIB/'WeReadNativeAuth.cpp').read_text()
        source = re.sub(r'^#include.*\n', '', source, flags=re.M)
        store = (LIB/'WeReadStore.cpp').read_text()
        atomic = store[store.index('bool atomicReplace('):store.index('bool looksLikeZip(')]
        code = r'''
#include "WeReadNativeAuthProtocol.h"
#include <cassert>
#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>
uint32_t tick=0,epoch=1700056800;
unsigned long millis() { return tick; }
uint32_t esp_random() { return 12345678; }
namespace TimeUtils { uint32_t getCurrentValidTimestamp() { return epoch; } }
namespace WeReadClient { enum class Error { Ok, Clock, Network, SessionExpired, Protocol, LoginFailed, SdCard }; }
namespace WeReadNativeTime {
 const char* rootCA() { return "verified-test-root"; }
 bool hash256(const uint8_t*,size_t,char out[65]) { memset(out,'a',64);out[64]=0;return true; }
}
std::map<std::string,std::string> files;
bool writeFails=false,corruptWrite=false,renameFails=false,online=true;
struct HalFile {
 std::string path; size_t offset=0;
 size_t fileSize64() { return files[path].size(); }
 int read(void* p,size_t n) { auto& s=files[path]; n=std::min(n,s.size()-offset);memcpy(p,s.data()+offset,n);offset+=n;return n; }
 size_t write(const uint8_t* p,size_t n) { if(writeFails)return 0;files[path].assign((const char*)p,n);if(corruptWrite)files[path][10]='X';return n; }
 void flush() {}
};
struct {
 bool exists(const char* p) { return files.count(p); }
 bool remove(const char* p) { return files.erase(p); }
 bool rename(const char* a,const char* b) {
  if(renameFails&&strstr(a,".part"))return false;
  if(!files.count(a)||files.count(b))return false;
  files[b]=files[a];files.erase(a);return true;
 }
 bool openFileForRead(const char*,const char* p,HalFile& f) { if(!files.count(p))return false;f.path=p;return true; }
 bool openFileForWrite(const char*,const char* p,HalFile& f) { f.path=p;files[p]="";return true; }
} Storage;
namespace WeReadStore { bool ensureRoot() { return true; }
''' + atomic + r'''
}
namespace WeReadHttpClient {
 enum class Result { Ok, NetworkError, Aborted };
 struct Session { void reset() {} };
 struct Header { const char* name; const char* value; };
 struct RequestOptions {
  const char* method="GET"; const uint8_t* body=nullptr; size_t bodySize=0;
  const Header* headers=nullptr; size_t headerCount=0; int timeoutMs=60000; bool redactUrl=false;
  const char* rootCA=nullptr; uint8_t* readBuffer=nullptr; size_t readBufferSize=0;
 };
 using DataCallback=std::function<bool(const uint8_t*,size_t)>;
 using HeaderCallback=std::function<void(const char*,const char*)>;
 struct Response { std::string match,json; int status=200; Result result=Result::Ok; bool auth=false,post=false; };
 std::vector<Response> responses; unsigned calls=0; std::string lastBody;
 bool networkReady() { return online; }
 Result request(Session&,const char* url,const RequestOptions& o,const DataCallback& cb,const HeaderCallback&,int& status) {
  assert(calls<responses.size());const auto r=responses[calls++];
  assert(strstr(url,r.match.c_str()));assert(!strstr(url,"batchUploadProgress"));
  assert(std::string(o.rootCA)=="verified-test-root"&&o.redactUrl);
  assert(o.headerCount==(r.auth?9:7));assert(std::string(o.method)==(r.post?"POST":"GET"));
  if(o.body)lastBody.assign((const char*)o.body,o.bodySize);
  status=r.status;
  if(r.result!=Result::Ok)return r.result;
  for(size_t i=0;i<r.json.size();i+=7) if(!cb((const uint8_t*)r.json.data()+i,std::min(size_t(7),r.json.size()-i)))return Result::Aborted;
  return Result::Ok;
 }
}
''' + header + source + r'''
using namespace WeReadNativeAuth;
using E=WeReadClient::Error;
using Event=Login::Event;
using Failure=Login::Failure;
using namespace WeReadHttpClient;
const char* path="/.crosspoint/weread/native-session.json";
const std::string old=R"({"vid":"123","accessToken":"old-token","deviceId":"test-device","installId":"test-install"})";
E error;
void reset() { files.clear();responses.clear();calls=0;tick=0;epoch=1700056800;writeFails=corruptWrite=renameFails=false;online=true; }
void qrResponses() {
 responses.push_back({"/wxticket?",R"({"timeStamp":"1700056800","signature":"testsig"})"});
 responses.push_back({"/connect/sdk/qrconnect?",R"({"uuid":"test-uuid"})"});
}
void showQr(Login& login) {
 assert(login.begin("123","456"));assert(login.step(error)==Event::None);
 assert(login.step(error)==Event::None);assert(login.step(error)==Event::QrReady);
 assert(std::string(login.qrUrl())=="https://open.weixin.qq.com/connect/confirm?uuid=test-uuid");
 assert(!login.readyToStep());assert(login.step(error)==Event::None);assert(calls==2);tick=1500;
}
void exchangeResponses(const char* vid="123") {
 responses.push_back({"/connect/l/qrconnect?",R"({"wx_errcode":405,"wx_code":"test-code"})"});
 responses.push_back({"/login",std::string("{\"vid\":\"")+vid+"\",\"accessToken\":\"new-token\",\"refreshToken\":\"unused\"}",200,Result::Ok,false,true});
}
int main() {
 // Existing imported credential stays compatible; checking never invokes login or an upload.
 reset(); files[path]=old;responses.push_back({"/book/getProgress?bookId=456",R"({"bookId":"456"})",200,Result::Ok,true});
 { Login l;assert(l.begin("123","456"));assert(l.step(error)==Event::None);assert(l.step(error)==Event::Complete);
   assert(calls==1&&files[path]==old);assert(l.step(error)==Event::Complete&&calls==1); }
 // Missing credential -> waiting -> timeout -> scan -> phone confirmation -> saved once.
 reset();qrResponses();
 responses.push_back({"/connect/l/qrconnect?",R"({"wx_errcode":408})"});
 responses.push_back({"/connect/l/qrconnect?","",200,Result::NetworkError});
 responses.push_back({"/connect/l/qrconnect?",R"({"wx_errcode":404})"});exchangeResponses();
 { Login l;showQr(l);assert(l.step(error)==Event::None);tick+=1500;assert(l.step(error)==Event::None);
   tick+=1500;assert(l.step(error)==Event::Scanned);tick+=1500;
   assert(l.step(error)==Event::None);assert(l.step(error)==Event::None);assert(l.step(error)==Event::Complete);
   assert(files[path].find("new-token")!=std::string::npos);assert(files[path].find("refreshToken")==std::string::npos);
   assert(lastBody.find("test-code")!=std::string::npos);assert(calls==7); }
 // Expired imported credential opens QR. A transport/protocol failure does not discard it.
 for(int scenario=0;scenario<3;++scenario) {
  reset();files[path]=old;
  responses.push_back({"/book/getProgress?",scenario==2?R"({"errCode":-9000})":R"({"errCode":-2012})",
                        200,scenario==1?Result::NetworkError:Result::Ok,true});
  if(scenario==0)qrResponses();
  Login l;assert(l.begin("123","456"));assert(l.step(error)==Event::None);
  if(scenario==0) { assert(l.step(error)==Event::None);assert(l.step(error)==Event::None);assert(l.step(error)==Event::QrReady); }
  else { assert(l.step(error)==Event::Failed);assert(l.step(error)==Event::Failed);assert(calls==1); }
  assert(files[path]==old);
 }
 // Wrong-account login, failed/short write, bad readback and failed final rename preserve old session.
 for(int scenario=0;scenario<5;++scenario) {
  reset();qrResponses();exchangeResponses(scenario==0?"999":"123");
  Login l;showQr(l);files[path]=old;
  assert(l.step(error)==Event::None);
  if(scenario==0) { assert(l.step(error)==Event::Failed);assert(l.failure()==Failure::AccountMismatch); }
  else {
   assert(l.step(error)==Event::None);
   writeFails=scenario==1;corruptWrite=scenario==2;renameFails=scenario==3;
   if(scenario==4) { assert(l.step(error)==Event::Complete);assert(files[path]!=old);continue; }
   assert(l.step(error)==Event::Failed);assert(error==E::SdCard);
  }
  assert(files[path]==old);assert(calls==4);assert(l.step(error)==Event::Failed&&calls==4);
 }
 // QR expiry, denied login, malformed reply, Wi-Fi disconnect and cancellation cannot save/report.
 for(int scenario=0;scenario<5;++scenario) {
  reset();qrResponses();Login l;showQr(l);
  if(scenario==0)tick=300000;
  else if(scenario==1)responses.push_back({"/connect/l/qrconnect?",R"({"wx_errcode":403})"});
  else if(scenario==2)responses.push_back({"/connect/l/qrconnect?",R"({"wx_code":"oops"})"});
  else if(scenario==3) { online=false;responses.push_back({"/connect/l/qrconnect?","",200,Result::NetworkError}); }
  else continue; // Destruction of the workspace is cancellation; no callbacks or workers outlive it.
  assert(l.step(error)==Event::Failed);assert(files.empty());
 }
 // A lost login response is never retried with the same authorization code.
 reset();qrResponses();exchangeResponses();responses.back().result=Result::NetworkError;
 { Login l;showQr(l);assert(l.step(error)==Event::None);assert(l.step(error)==Event::Failed);
   assert(l.step(error)==Event::Failed&&calls==4&&files.empty()); }
 // Recover previous complete session after interruption between replacement renames.
 reset();files[std::string(path)+".bak"]=old;
 responses.push_back({"/book/getProgress?",R"({"bookId":"456"})",200,Result::Ok,true});
 { Login l;assert(l.begin("123","456"));assert(l.step(error)==Event::None);assert(l.step(error)==Event::Complete);assert(files[path]==old); }
 // No clock: no network or disk mutation. Counter wrap still expires QR after five minutes.
 reset();epoch=0;
 { Login l;assert(l.begin("123","456"));assert(l.step(error)==Event::Failed);assert(error==E::Clock&&calls==0); }
 reset();qrResponses();tick=UINT32_MAX-1000;
 { Login l;assert(l.begin("123","456"));l.step(error);l.step(error);assert(l.step(error)==Event::QrReady);
   tick+=300000;assert(l.readyToStep());assert(l.step(error)==Event::Failed&&l.failure()==Failure::Expired); }
}
'''
        compile_run(code, [LIB/'WeReadNativeAuthProtocol.cpp', LIB/'WeReadNativeProtocol.cpp',
                           ROOT/'lib/JsonParser/StreamingJsonParser.cpp'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
