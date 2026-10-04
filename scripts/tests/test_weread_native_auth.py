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
                            for name in ('startSync', 'startNativeLogin', 'advanceNativeLogin', 'startTimeSync', 'advanceTimeSync', 'advanceSync'))
        compile_run(r'''
#include <atomic>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <memory>
#include <utility>
#define LOG_ERR(...)
#define LOG_INF(...)
namespace WeReadProtocol { struct RemoteProgress { float percent=0; }; }
namespace WeReadClient {
 enum class Error { Ok, Clock, OutOfMemory, Protocol, Network, SessionExpired, SdCard };
 enum class ProgressSyncMode { Compare, UploadLocal };
 enum class ProgressSyncOutcome { Pending, SelectionRequired, ApplyRemote, AlreadySynced };
 inline unsigned webLive=0;
 struct Operation {
  Operation(){++webLive;}~Operation(){--webLive;}
  enum class Event { None, Authenticated, DetailReady, ChapterComplete, QrReady, Cancelled, Failed, Complete };
  struct Result { ProgressSyncOutcome outcome=ProgressSyncOutcome::AlreadySynced;WeReadProtocol::RemoteProgress remote; };
  Event event=Event::None; Error err=Error::Ok;unsigned starts=0,resets=0;
  bool beginProgressSync(const char*,int,ProgressSyncMode) { ++starts;return true; }
  Event step() { return event; } Error error() const { return err; }
  void reset() { ++resets; } Result progressSyncResult() { return {}; }
  const char* nativeTimeDiagnostic() { return "U6 N7 T9:-125 H200 E0 V3"; }
 };
}
using E=WeReadClient::Error;
bool allocationFails=false,clockValid=true;
unsigned authLive=0,uploadLive=0;
namespace WeReadNativeTime {
 struct Upload {
  E err=E::Ok;bool done=true;
  Upload(){++uploadLive;assert(!WeReadClient::webLive&&!authLive);}
  ~Upload(){--uploadLive;}
  template<class T> bool begin(const char*,T&){return true;}
  bool step(E& e){e=err;return done;}
  void diagnosticCode(char* out,size_t n){snprintf(out,n,"U6 N7 T9:-125 H200 E0 V3");}
 };
}
namespace WeReadNativeAuth {
 struct Login {
  enum class Event { None, QrReady, Scanned, Complete, Failed };
  enum class Failure { None, AccountMismatch };
  enum class Phase { Load, Verify, Ticket, Qr, Poll, Exchange, Save, Done, Failed };
  Phase failurePhase() { return Phase::Qr; }
  void diagnosticCode(char* out,size_t n) { snprintf(out,n,"A3 N3 T6:-188 H-1 E0"); }
  Event event=Event::None; bool ready=true,forced=false; E err=E::Ok;
  Login() { assert(!WeReadClient::webLive&&!uploadLive);++authLive; } ~Login() { --authLive; }
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
 std::unique_ptr<WeReadClient::Operation> operation_;
 std::unique_ptr<WeReadNativeTime::Upload> nativeUpload_;
 struct { bool healthy(){return true;}const char* account() { return "123"; } unsigned pending=300,uncertain=0;unsigned pendingSeconds() { return pending; } } timeLedger_;
 std::unique_ptr<WeReadNativeAuth::Login> nativeLogin_;
 WeReadNativeAuth::Login::Failure authFailure_=WeReadNativeAuth::Login::Failure::None;
 WeReadNativeAuth::Login::Phase authPhase_=WeReadNativeAuth::Login::Phase::Load;
 char authDiagnostic_[96]={};
 char timeDiagnostic_[96]={};
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
 void startSync();void startNativeLogin();void advanceNativeLogin();void advanceSync();void startTimeSync();void advanceTimeSync();
};
''' + methods + r'''
int main() {
 using S=WeReadProgressSyncActivity::State;
 using A=WeReadNativeAuth::Login;
 using O=WeReadClient::Operation;
 { WeReadProgressSyncActivity p;p.startSync();assert(p.state_==S::Authenticating&&authLive==1&&!p.operation_);
   p.nativeLogin_->event=A::Event::QrReady;p.advanceNativeLogin();assert(p.state_==S::AuthQr&&p.nativeQrUrl_[0]);
   p.nativeLogin_->event=A::Event::Scanned;p.advanceNativeLogin();assert(p.state_==S::AuthScanned);
   p.nativeLogin_->event=A::Event::Complete;p.advanceNativeLogin();assert(p.state_==S::Starting&&p.nativeReady_&&!authLive);
   p.startSync();assert(p.state_==S::Syncing&&p.operation_->starts==1&&!authLive);
   p.operation_->event=O::Event::Complete;p.advanceSync();assert(p.state_==S::TimeStarting&&!p.operation_&&!WeReadClient::webLive);
   p.startTimeSync();assert(p.nativeUpload_&&uploadLive==1&&!WeReadClient::webLive&&!authLive);p.nativeUpload_->done=false;p.advanceTimeSync();assert(p.nativeUpload_);
   p.timeLedger_.pending=0;p.nativeUpload_->done=true;p.advanceTimeSync();assert(p.state_==S::Success&&!uploadLive);
 }
 { WeReadProgressSyncActivity p;allocationFails=true;p.startSync();assert(p.state_==S::AuthFailed&&p.error_==E::OutOfMemory&&!authLive);allocationFails=false; }
 { WeReadProgressSyncActivity p;p.startSync();p.nativeLogin_->event=A::Event::Failed;p.nativeLogin_->err=E::Protocol;
   p.advanceNativeLogin();assert(p.state_==S::AuthFailed&&!authLive&&!p.nativeReady_&&!p.operation_);
   assert(p.authPhase_==A::Phase::Qr && strstr(p.authDiagnostic_,"T6:-188")); }
 { WeReadProgressSyncActivity p;p.startSync();p.nativeLogin_->ready=false;
   p.advanceNativeLogin();assert(p.state_==S::Authenticating); } // Cancellation releases workspace.
 assert(!authLive);
 { WeReadProgressSyncActivity p;p.radioStopped_=true;p.startSync();assert(p.wifiChild&&!authLive&&!p.operation_); }
 { WeReadProgressSyncActivity p;clockValid=false;p.startSync();assert(p.state_==S::Failed&&!authLive);clockValid=true; }
 // Mid-upload expiry gets exactly one automatic login; uncertain duration is not made pending again.
 { WeReadProgressSyncActivity p;p.state_=S::TimeSyncing;p.nativeReady_=true;
   p.startTimeSync();p.timeLedger_.pending=0;p.timeLedger_.uncertain=300;p.nativeUpload_->err=E::SessionExpired;
   p.advanceTimeSync();assert(p.state_==S::Starting&&p.forceNativeLogin_&&p.nativeRecoveryAttempted_&&!p.nativeReady_);
   assert(p.timeLedger_.pending==0&&p.timeLedger_.uncertain==300);
   p.startSync();assert(p.nativeLogin_->forced);
   p.nativeLogin_->event=A::Event::Complete;p.advanceNativeLogin();assert(!authLive);
   p.nativeUpload_=std::make_unique<WeReadNativeTime::Upload>();p.nativeUpload_->err=E::SessionExpired;p.state_=S::TimeSyncing;p.advanceTimeSync();assert(p.state_==S::TimeFailed&&!authLive);assert(strstr(p.timeDiagnostic_,"U6 N7"));
 }
 { WeReadProgressSyncActivity p;p.nativeReady_=true;allocationFails=true;p.startSync();assert(p.state_==S::Failed&&!p.operation_);allocationFails=false; }
 { WeReadProgressSyncActivity p;allocationFails=true;p.startTimeSync();assert(p.state_==S::TimeFailed&&!p.nativeUpload_);allocationFails=false; }
 assert(!authLive&&!uploadLive&&!WeReadClient::webLive);
 // Web-session expiry still uses its own login path; native QR cannot replace Web cookies.
 { WeReadProgressSyncActivity p;p.state_=S::Syncing;p.operation_=std::make_unique<O>();p.operation_->event=O::Event::Failed;p.operation_->err=E::SessionExpired;
   p.advanceSync();assert(p.state_==S::LoginRequired&&!p.nativeRecoveryAttempted_); }
}
''')

    def test_actual_http_framing_preserves_status_and_failure_stage(self):
        header = re.sub(r'^#(?:include.*|pragma once)\n', '', (LIB/'WeReadHttpClient.h').read_text(), flags=re.M)
        source = re.sub(r'^#include.*\n', '', (LIB/'WeReadHttpClient.cpp').read_text(), flags=re.M)
        compile_run(r"""
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <cctype>
#include <algorithm>
#include <limits>
#include <functional>
#include <string>
#include <cassert>
#define FREEINK_NET_WOLFSSL 1
#define CROSSPOINT_VERSION "test"
#define LOG_INF(...)
#define LOG_ERR(...)
#define LOG_DBG(...)
using wifi_mode_t=int;
constexpr int WIFI_MODE_STA=1,WL_CONNECTED=1;
bool online=true;int fault=0;unsigned tick=0;
unsigned millis(){return ++tick;}void delay(unsigned n){tick+=n;}
struct { int getMode(){return online?1:0;}int status(){return online?1:0;} } WiFi;
struct { unsigned getFreeHeap(){return 50000;}unsigned getMaxAllocHeap(){return 24000;} } ESP;
std::string wire;unsigned writes=0,gates=0;bool gateOk=true;
namespace freeink {
 struct SecureClient {
  size_t offset=0;bool active=false;int stage=0,err=0;
  void setCACert(const char* c){assert(!strcmp(c,"root"));}void setInsecure(){assert(false);}
  void setTimeout(unsigned){}bool connect(const char*,uint16_t){offset=0;if(fault==1){stage=6;err=-188;return false;}return active=true;}
  bool connected(){return active&&offset<wire.size();}void stop(){active=false;}
  int available(){return active?wire.size()-offset:0;}
  size_t write(const uint8_t*,size_t n){++writes;if(fault==2){stage=8;err=-125;return 0;}return n;}
  int read(){return connected()?static_cast<unsigned char>(wire[offset++]):-1;}
  int read(uint8_t* b,size_t n){if(!connected())return -1;n=std::min(n,wire.size()-offset);memcpy(b,wire.data()+offset,n);offset+=n;return n;}
  unsigned lastErrorStage(){return stage;}int lastError(){return err;}int firstHandshakeError(){return fault==1?-125:0;}
 };
}
""" + header + source + r"""
int main(){
 using namespace WeReadHttpClient;
 Diagnostic d;uint8_t io[1024];RequestOptions o;o.rootCA="root";o.readBuffer=io;o.readBufferSize=sizeof(io);o.diagnostic=&d;
 const char* url="https://i.weread.qq.com/book/getProgress?bookId=456";int status=0;size_t received=0,maxChunk=0;
 DataCallback accept=[&](const uint8_t*,size_t n){received+=n;maxChunk=std::max(maxChunk,n);return true;};
 auto run=[&](DataCallback cb){Session s;return request(s,url,o,cb,{},status);};
 wire="HTTP/1.1 401 LOGIN ERR\r\nContent-Length: 100\r\n\r\n{}";
 assert(run(accept)==Result::NetworkError&&status==401&&d.stage==RequestStage::Body);
 wire="HTTP/1.1 403 FORBIDDEN\r\nContent-Length: 2\r\n\r\n{}";
 assert(run([](const uint8_t*,size_t){return false;})==Result::Aborted&&status==403&&d.stage==RequestStage::Body);
 wire="HTTP/1.1 200 OK\r\nContent-Length: 68114\r\nConnection: close\r\n\r\n"+std::string(68114,'x');
 received=maxChunk=0;assert(run(accept)==Result::Ok&&status==200&&received==68114&&maxChunk<=1024&&d.stage==RequestStage::Complete);
 wire="HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n";
 received=0;assert(run(accept)==Result::Ok&&received==2);
 fault=1;assert(run(accept)==Result::NetworkError&&status==-1&&d.stage==RequestStage::Connect);
 assert(d.tlsStage==6&&d.tlsError==-188&&d.firstTlsError==-125&&d.freeBefore==50000&&d.largestBefore==24000);
 fault=2;assert(run(accept)==Result::NetworkError&&d.stage==RequestStage::Write&&d.tlsError==-125);
 fault=0;wire="";assert(run(accept)==Result::NetworkError&&d.stage==RequestStage::Status);
 wire="HTTP/1.1 200 OK\r\nContent-Length:";assert(run(accept)==Result::NetworkError&&status==200&&d.stage==RequestStage::Headers);
 wire="HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}";
 o.beforeSend=[](void* ctx){assert(ctx==&gateOk);assert(writes==0);++gates;return gateOk;};o.beforeSendContext=&gateOk;
 fault=1;writes=gates=0;assert(run(accept)==Result::NetworkError&&!gates&&!writes);
 fault=0;gateOk=false;assert(run(accept)==Result::Aborted&&gates==1&&!writes);
 gateOk=true;gates=0;assert(run(accept)==Result::Ok&&gates==1&&writes>0);
 fault=2;writes=gates=0;assert(run(accept)==Result::NetworkError&&gates==1&&writes==1);
 writes=gates=0;
 online=false;assert(run(accept)==Result::NetworkError&&status==-1&&d.stage==RequestStage::Wifi&&d.tlsError==0&&!writes&&!gates);
}
""")

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
 r={}; assert(parse(r,R"({"errcode":-2012,"errlog":"expired","errmsg":"expired"})")); assert(r.errorCode==-2012);
 for(const char* bad:{R"({"vid":true})",R"({"vid":null})",R"({"vid":["123"]})",
                      R"({"vid":{"vid":"123"}})",R"({"vid":"123","vid":"456"})",
                      R"([{"vid":"123"}])",R"({"wx_errcode":405.1})",R"({"wx_errcode":999999999999})",
                      R"({"vid":"123"}) {}",R"({"vid":"123" )",R"({"vid":"12\u00003"})"}) {
  r={}; if(parse(r,bad)) { fprintf(stderr,"Unexpected accepted JSON: %s\n",bad); assert(false); }
 }
 r={}; assert(!parse(r,"{\"accessToken\":\""+std::string(600,'a')+"\"}"));
 r={}; assert(parse(r,"{\"qrcode\":{\"qrcodebase64\":\""+std::string(68000,'a')+"\"},\"uuid\":\"ok\"}"));
 r={}; assert(parse(r,R"({"nested":{"vid":"123","wx_errcode":405}})"));
 assert(!r.credentials.vid[0]&&!r.hasQrStatus);
}
''', [LIB/'WeReadNativeAuthProtocol.cpp', LIB/'WeReadNativeProtocol.cpp',
      ROOT/'lib/JsonParser/StreamingJsonParser.cpp'], crypto=True)
        body = json.loads(result)
        self.assertEqual(body['signature'], hashlib.sha256(b'1700056800123test-device20').hexdigest())
        self.assertEqual(body['code'], 'test-code')
        self.assertEqual(body['deviceType'], 3)

    def test_opaque_refresh_token_does_not_reject_valid_login(self):
        compile_run(r'''
#include "WeReadNativeAuthProtocol.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <string>
using namespace WeReadNativeAuth;
bool parse(Reply& r,const std::string& s,size_t chunk) {
 StreamingJsonParser p(r.callbacks());
 for(size_t i=0;i<s.size();i+=chunk) {
  p.feed(s.data()+i,std::min(chunk,s.size()-i));
  if(p.hasError()||r.invalid)return false; // Same early-abort rule as Login::request.
 }
 p.feed(" ",1);return !p.hasError()&&!r.invalid&&r.closed;
}
int main() {
 const std::string required=R"("vid":123,"accessToken":"test-token","deviceId":"test-device","installId":"test-install")";
 // Synthetic values reproduce the real '@' shape without publishing the private token.
 // Unused optional values may be absent, long, null or nested; required fields stay strict.
 for(const std::string& refresh:{std::string(R"("opaque@refresh-token")"),std::string("null"),std::string("false"),
      std::string("123"),std::string(R"({"accessToken":"ignored@value","vid":999})"),
      std::string(R"(["unused",null,{"vid":999}])"),"\""+std::string(2048,'@')+"\""}) {
  for(size_t chunk:{size_t(1),size_t(7),size_t(512),size_t(1024)}) {
   for(bool first:{false,true}) {
    const std::string optional="\"refreshToken\":"+refresh;
    Reply r;assert(parse(r,"{"+(first?optional+","+required:required+","+optional)+"}",chunk));
    assert(validCredentials(r.credentials,"123")&&!r.credentials.refreshToken[0]);
    char saved[1024];assert(sessionJson(r.credentials,"123",saved,sizeof(saved)));
    assert(!strstr(saved,"refreshToken")&&!strchr(saved,'@'));
   }
  }
 }
 for(const char* bad:{R"({"vid":123,"accessToken":"bad@token","refreshToken":"opaque@token"})",
     R"({"vid":123,"accessToken":null,"refreshToken":"opaque@token"})",
     R"({"vid":123,"accessToken":"one","accessToken":"two","refreshToken":"opaque@token"})",
     R"({"vid":123,"refreshToken":"unterminated})",
     R"({"signature":"bad@signature","refreshToken":"opaque@token"})"}) {
  Reply r;assert(!parse(r,bad,7));
 }
 Reply r;assert(parse(r,R"({"vid":123,"refreshToken":"opaque@token"})",7));
 assert(!validCredentials(r.credentials,"123"));
}
''', [LIB/'WeReadNativeAuthProtocol.cpp', LIB/'WeReadNativeProtocol.cpp',
      ROOT/'lib/JsonParser/StreamingJsonParser.cpp'])

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
#define LOG_ERR(...)
#define CROSSPOINT_VERSION "test-version"
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
 enum class RequestStage { None,Wifi,Setup,Connect,Write,Status,Headers,Body,Complete };
 struct Diagnostic { RequestStage stage=RequestStage::None;uint8_t tlsStage=0;int tlsError=0,firstTlsError=0;
  uint32_t freeBefore=0,largestBefore=0,freeAfter=0,largestAfter=0; };
 struct Session { void reset() {} };
 struct Header { const char* name; const char* value; };
 struct RequestOptions {
  const char* method="GET"; const uint8_t* body=nullptr; size_t bodySize=0;
  const Header* headers=nullptr; size_t headerCount=0; int timeoutMs=60000; bool redactUrl=false;
  const char* rootCA=nullptr; uint8_t* readBuffer=nullptr; size_t readBufferSize=0;Diagnostic* diagnostic=nullptr;
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
  status=r.status;assert(o.diagnostic);
  o.diagnostic->stage=RequestStage::Connect;o.diagnostic->tlsStage=6;o.diagnostic->tlsError=-188;
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
const char* diagnosticPath="/.crosspoint/weread/native-auth-error.txt";
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
 responses.push_back({"/login",std::string("{\"vid\":\"")+vid+"\",\"accessToken\":\"new-token\",\"refreshToken\":\"opaque@refresh-token\"}",200,Result::Ok,false,true});
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
 // HTTP 401/403 must open QR even when the error body aborts/truncates.
 for(int status:{401,403}) for(Result transport:{Result::Ok,Result::NetworkError,Result::Aborted}) {
  reset();files[path]=old;responses.push_back({"/book/getProgress?",R"({"errcode":-2012})",status,transport,true});qrResponses();
  Login l;assert(l.begin("123","456"));assert(l.step(error)==Event::None);
  assert(l.step(error)==Event::None);assert(l.step(error)==Event::None);assert(l.step(error)==Event::QrReady);
  assert(files[path]==old&&!files.count(diagnosticPath));
 }
 // A malformed HTTP 200 is a protocol failure, not a transient poll timeout.
 reset();qrResponses();
 { Login l;showQr(l);responses.push_back({"/connect/l/qrconnect?",R"({"wx_errcode":true})"});
   assert(l.step(error)==Event::Failed&&error==E::Protocol);
   assert(l.failurePhase()==Login::Phase::Poll);
   char code[96];l.diagnosticCode(code,sizeof(code));assert(strstr(code,"A4")&&strstr(code,"T6:-188"));
   const auto report=files[diagnosticPath];assert(report.find("tls_error=-188")!=std::string::npos);
   for(const char* secret:{"old-token","test-code","test-uuid","test-device","test-install","accessToken","http://","https://"})
    assert(report.find(secret)==std::string::npos);
   const auto snapshot=files;assert(l.step(error)==Event::Failed&&files==snapshot);
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
  assert(l.step(error)==Event::Failed);assert(!files.count(path));
 }
 // A lost login response is never retried with the same authorization code.
 reset();qrResponses();exchangeResponses();responses.back().result=Result::NetworkError;
 { Login l;showQr(l);assert(l.step(error)==Event::None);assert(l.step(error)==Event::Failed);
   assert(l.step(error)==Event::Failed&&calls==4&&!files.count(path)); }
 // Recover previous complete session after interruption between replacement renames.
 reset();files[std::string(path)+".bak"]=old;
 responses.push_back({"/book/getProgress?",R"({"bookId":"456"})",200,Result::Ok,true});
 { Login l;assert(l.begin("123","456"));assert(l.step(error)==Event::None);assert(l.step(error)==Event::Complete);assert(files[path]==old); }
 // No clock: no network or session mutation. Counter wrap still expires QR after five minutes.
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
