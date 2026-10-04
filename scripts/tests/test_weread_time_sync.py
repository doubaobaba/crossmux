"""Host-run production C++ native payload, parser, and non-replay state-machine checks.
No credentials and no network. Signature fixtures originate from the official ARM64 library.
"""
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / 'lib/WeReadWebApi/src'


def compile_run(code, sources=(), crypto=False):
    with tempfile.TemporaryDirectory(prefix='weread-native-test-') as tmp:
        source = Path(tmp) / 'test.cpp'
        source.write_text(code)
        binary = Path(tmp) / 'test'
        flags = ['-std=c++20', '-Wall', '-Wextra', '-Wno-deprecated-declarations', '-I'+str(LIB),
                 '-I'+str(ROOT/'lib/JsonParser')]
        subprocess.run(shlex.split(os.environ.get('CXX', 'c++')) + flags + [str(source)] +
                       list(map(str, sources)) + (['-lcrypto'] if crypto and sys.platform != 'darwin' else []) +
                       ['-o', str(binary)], check=True)
        return subprocess.check_output([str(binary)], text=True)


class WeReadTimeSyncTest(unittest.TestCase):
    def test_original_native_signatures_and_full_hour_payload(self):
        fixtures = json.loads((ROOT/'test/weread_webapi/native_signatures.json').read_text())
        checks = []
        for parts, expected in fixtures:
            checks.append('{ const char* p[4]={' + ','.join(json.dumps(s) for s in parts) + '};'
                          'assert(sign(p,s,hash,out)); assert(!strcmp(out,' + json.dumps(expected) + ')); }')
        code = r'''
#include "WeReadNativeProtocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
using namespace WeReadNativeProtocol;
bool hash(const uint8_t* data,size_t n,char out[65]) {
  uint8_t d[32];
#ifdef __APPLE__
  CC_SHA256(data,n,d);
#else
  SHA256(data,n,d);
#endif
  for (unsigned i=0;i<32;++i) snprintf(out+i*2,3,"%02x",d[i]);
  return true;
}
int main() {
  Scratch s; char out[65];
''' + '\n'.join(checks) + r'''
  Credentials c; strcpy(c.vid,"1234"); strcpy(c.deviceId,"test-device"); strcpy(c.installId,"test-install");
  Position p; p.chapterUid=2; p.chapterIdx=1; p.chapterOffset=900; p.progress=13; p.currentProgress=12;
  WeReadTimeLedger::Hour hours[16]; for (unsigned i=0;i<16;++i) hours[i]={1699999200+i*3600,3600};
  char body[4096];
  assert(batch(c,"9988",1,p,hours,16,"5ecdcfd7f","TEST-TOKEN",1700056800,20,s,hash,body,sizeof(body)));
  puts(body);
  assert(!batch(c,"9988",1,p,hours,16,"5ecdcfd7f","TEST-TOKEN",1700056800,20,s,hash,body,50));
  hours[1]=hours[0];
  assert(!batch(c,"9988",1,p,hours,16,"5ecdcfd7f","TEST-TOKEN",1700056800,20,s,hash,body,sizeof(body)));
  hours[0]={1699999200,3601};
  assert(!batch(c,"9988",1,p,hours,1,"5ecdcfd7f","TEST-TOKEN",1700056800,20,s,hash,body,sizeof(body)));
}
'''
        result=json.loads(compile_run(code,[LIB/'WeReadNativeProtocol.cpp'],crypto=True))
        b=result['books'][0]
        self.assertEqual(b['readingTime'],57600)
        self.assertEqual(b['chapterOffset'],900)
        self.assertEqual(b['chapterUid'],2)
        self.assertEqual(b['chapterIdx'],1)
        self.assertEqual(b['progress'],13)
        self.assertEqual(b['currentProgress'],12)
        self.assertEqual(result['timestamp'],1700056800000)
        self.assertEqual(result['signature'],hashlib.sha256(b'1700056800000TEST-TOKEN20').hexdigest())
        self.assertEqual(b['signature'],'5a165ac9b539005bb7617fe3c95f8daa22011d40114aceeda67a296d4fb67387')

    def test_native_response_paths_and_bounds(self):
        source=(LIB/'WeReadNativeTime.cpp').read_text()
        header=(LIB/'WeReadNativeTime.h').read_text()
        reply=header[header.index('struct Reply {'):header.index('// One bounded')]
        helpers=source[source.index('bool copy('):source.index('bool sha256(')]
        code=r'''
#include "WeReadNativeProtocol.h"
#include <StreamingJsonParser.h>
#include <cassert>
#include <cstring>
using namespace WeReadNativeProtocol;
'''+reply+helpers+r'''
JsonCallbacks Reply::callbacks() { return {this,onKey,value,value,nullptr,nullptr,start,end,start,end,chunks}; }
bool parse(Reply& r,const char* text) { StreamingJsonParser p(r.callbacks()); p.feed(text,strlen(text)); p.feed(" ",1); return !p.hasError()&&!r.invalid&&r.closed; }
int main() {
  Reply r; assert(parse(r,R"({"bookId":"123","book":{"readingTime":300,"chapterUid":0,"chapterOffset":0,"progress":0},"progress":99})"));
  assert(r.hasReadingTime && r.readingTime==300 && r.positionFields==7 && r.position.progress==0);
  r={}; assert(parse(r,R"({"vid":123,"accessToken":"test-token","deviceId":"test-device","installId":"test-install"})"));
  assert(!strcmp(r.credentials.vid,"123"));
  r={}; assert(parse(r,R"({"feature":{"guest_token":"new-token"}})")); assert(!strcmp(r.guestToken,"new-token"));
  r={}; assert(parse(r,R"({"errCode":-2012})")); assert(r.errorCode==-2012);
  r={}; assert(parse(r,R"({"succ":1})")); assert(r.succeeded);
  r={}; assert(parse(r,R"({"succ":0})")); assert(!r.succeeded);
  r={}; assert(!parse(r,R"({"succ":1)"));
  r={}; assert(!parse(r,R"({"book":{"readingTime":-1}})"));
  r={}; assert(!parse(r,R"({"book":{"chapterOffset":4294967296}})"));
  r={}; assert(!parse(r,R"({"book":{"chapterOffset":1.5}})"));
}
'''
        compile_run(code,[ROOT/'lib/JsonParser/StreamingJsonParser.cpp'])

    def test_upload_diagnostic_reports_phase_without_credentials(self):
        source=(LIB/'WeReadNativeTime.cpp').read_text()
        diagnostics=source[source.index('void Upload::diagnosticCode('):source.index('static_assert(sizeof(Upload)')]
        compile_run(r'''
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <initializer_list>
#define CROSSPOINT_VERSION "test-version"
enum class Error { Ok,Network };
namespace TimeUtils { uint32_t getCurrentValidTimestamp(){return 1700056800;} }
namespace WeReadStore { bool root=true;bool ensureRoot(){return root;} }
std::string saved;bool writable=true;unsigned opens=0,flushes=0;
struct HalFile {
 size_t write(const uint8_t* data,size_t n){saved.assign((const char*)data,n);return n;}
 void flush(){++flushes;}
};
struct {
 bool openFileForWrite(const char*,const char* path,HalFile&){
  ++opens;assert(!strcmp(path,"/.crosspoint/weread/native-time-error.txt"));return writable;
 }
} Storage;
struct Upload {
 unsigned failurePhase_=6;
 struct { unsigned stage=7,tlsStage=9;int tlsError=-125,firstTlsError=0;
          uint32_t freeBefore=20000,largestBefore=8000,freeAfter=21000,largestAfter=10000; } diagnostic_;
 int httpStatus_=200,transportResult_=1;size_t received_=1024;
 struct { int errorCode=0;bool invalid=false,closed=false,hasReadingTime=false;uint64_t readingTime=0; } reply_;
 bool parserError_=false,postAcknowledged_=true;uint8_t verifyAttempts_=3;uint64_t before_=1000;
 struct Ledger { uint32_t inFlightSeconds(){return 300;} } ledger;
 Ledger* ledger_=&ledger;
 char body_[4096]="SECRET-REQUEST";
 void diagnosticCode(char*,size_t)const;void saveDiagnostic(Error);
};
''' + diagnostics + r'''
int main(){
 Upload u;char code[96];u.diagnosticCode(code,sizeof(code));
 assert(!strcmp(code,"U6 N7 T9:-125 H200 E0 V3"));
 u.saveDiagnostic(Error::Network);assert(opens==1&&flushes==1);
 for(const char* expected:{"upload_phase=6\n","http_status=200\n","tls_error=-125\n",
      "post_acknowledged=1\n","verify_attempts=3\n","cloud_before=1000\n","inflight_seconds=300\n"})
  assert(saved.find(expected)!=std::string::npos);
 for(const char* secret:{"SECRET-REQUEST","accessToken","deviceId","vid=","https://"})
  assert(saved.find(secret)==std::string::npos);
 WeReadStore::root=false;u.saveDiagnostic(Error::Network);assert(opens==1);
 WeReadStore::root=true;writable=false;u.saveDiagnostic(Error::Network);assert(opens==2&&flushes==1);
}
''')

    def test_non_idempotent_post_and_verification_failures_never_replay(self):
        source=(LIB/'WeReadNativeTime.cpp').read_text()
        step=source[source.index('bool Upload::step('):source.index('void Upload::diagnosticCode(')]
        code=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
enum class Error { Ok, Clock, SessionExpired, Protocol, Unavailable, SdCard, Network };
uint32_t tick=0;uint32_t millis() { return tick; }
namespace TimeUtils { uint32_t getCurrentValidTimestamp() { return 1700056800; } }
long random(long,long) { return 20; }
bool token(const char*) { return true; }
bool sha256(const uint8_t*,size_t,char*) { return true; }
bool batch(...) { return true; }
struct WeReadTimeLedger {
 static constexpr unsigned kBatchHours=16;
 bool writable=true; unsigned pending=300,flight=0,accepted=0,uncertain=0;
 unsigned batchHours(int*,unsigned) { return 1; }
 bool prepareBatch() { if (!writable || !pending || flight) return false; flight=pending; pending=0; return true; }
 unsigned inFlightSeconds() { return flight; }
 bool acknowledgeBatch() { if (!writable) return false; accepted+=flight;flight=0; return true; }
 bool quarantineBatch() { if (!writable) return false; uncertain+=flight;flight=0; return true; }
};
struct Upload {
 enum class Phase { Load,Config,Feature,Info,Progress,Post,Verify,Done } phase_=Phase::Post;
 struct { unsigned resets=0;void reset() { ++resets; } } session_;
 struct { char configToken[256]="token",guestToken[128]={},bookId[64]="123"; unsigned version=1; bool succeeded=true,hasReadingTime=true; uint64_t readingTime=300; } reply_;
 WeReadTimeLedger ledger; WeReadTimeLedger* ledger_=&ledger;
 int credentials_=0,position_=0,scratch_=0,hours_[16];
 char bookId_[64]="123",body_[4096]={},token_[256]={},guest_[128]={};
 uint64_t before_=0; unsigned version_=1; int posts=0,gets=0; Error response=Error::Ok;
 bool loadCredentials() { return true; } bool resolvePosition() { return true; }
 Error request(const char*,bool post=false) { if(post)++posts;else++gets;return response; }
 Phase failurePhase_=Phase::Load;
 uint32_t nextVerify_=0;uint8_t verifyAttempts_=0;
 bool postAcknowledged_=false,verifyWaiting_=false;
 unsigned savedDiagnostics=0,diagnosticFlight=0;
 void saveDiagnostic(Error) { ++savedDiagnostics;diagnosticFlight=ledger.inFlightSeconds(); }
 bool step(Error&);
};
'''+step+r'''
bool advance(Upload& u,Error& error) {
 if(u.verifyWaiting_)tick=u.nextVerify_;
 return u.step(error);
}
int main() {
 Error error;
 Upload ok; assert(!ok.step(error)); assert(ok.posts==1 && ok.ledger.flight==300 && !ok.ledger.accepted);
 assert(ok.session_.resets==1 && ok.postAcknowledged_);
 assert(!ok.step(error) && ok.gets==0); // Initial confirmation delay is nonblocking.
 assert(advance(ok,error) && error==Error::Ok); assert(ok.ledger.accepted==300 && !ok.ledger.flight);
 assert(ok.step(error)); assert(ok.posts==1 && !ok.savedDiagnostics);
 for (Error e:{Error::Network,Error::Protocol,Error::SessionExpired}) {
  Upload u; u.response=e; assert(u.step(error)); assert(u.posts==1 && u.ledger.uncertain==300 && !u.ledger.pending);
  assert(u.savedDiagnostics==1 && u.diagnosticFlight==300 && u.failurePhase_==Upload::Phase::Post);
  u.response=Error::Ok; assert(u.step(error)); assert(u.posts==1 && !u.ledger.accepted && u.savedDiagnostics==1);
 }
 Upload no; no.ledger.writable=false; assert(no.step(error)); assert(no.posts==0 && no.ledger.pending==300);
 // Eventual consistency: first two reads lag, third matches. There is still one POST.
 Upload lag; assert(!lag.step(error)); lag.reply_.readingTime=0;
 assert(!advance(lag,error) && !lag.ledger.uncertain);assert(!advance(lag,error));
 lag.reply_.readingTime=300;assert(advance(lag,error)&&error==Error::Ok);
 assert(lag.posts==1&&lag.gets==3&&lag.ledger.accepted==300&&!lag.savedDiagnostics);
 // A transient read failure reconnects. No retry is allowed for the POST itself.
 Upload transient;assert(!transient.step(error));transient.response=Error::Network;
 assert(!advance(transient,error));assert(transient.ledger.flight==300&&!transient.ledger.uncertain);
 auto calls=transient.gets;assert(!transient.step(error)&&transient.gets==calls);
 transient.response=Error::Ok;assert(advance(transient,error));assert(transient.ledger.accepted==300&&transient.posts==1);
 // Bound attempts for both persistent network failure and stale stats, preserving non-replay.
 for(bool network:{false,true}) {
  Upload u; assert(!u.step(error));u.reply_.readingTime=0;
  if(network)u.response=Error::Network;
  assert(!advance(u,error));assert(!advance(u,error));assert(advance(u,error));
  assert(u.posts==1&&u.gets==3&&u.ledger.uncertain==300&&!u.ledger.accepted);
  assert(u.savedDiagnostics==1&&u.diagnosticFlight==300&&u.failurePhase_==Upload::Phase::Verify);
  assert(u.step(error)&&u.posts==1&&u.gets==3&&u.savedDiagnostics==1);
 }
 Upload rejected; rejected.reply_.succeeded=false; assert(rejected.step(error)); assert(rejected.ledger.uncertain==300);
 Upload wrongBook; assert(!wrongBook.step(error)); strcpy(wrongBook.reply_.bookId,"999");
 assert(advance(wrongBook,error)); assert(wrongBook.ledger.uncertain==300&&wrongBook.gets==1);
 for(Error e:{Error::Protocol,Error::SessionExpired}) {
  Upload u;assert(!u.step(error));u.response=e;assert(advance(u,error));assert(u.gets==1&&u.ledger.uncertain==300);
 }
 // Delay arithmetic works across millis() rollover.
 tick=UINT32_MAX-100;Upload wrap;assert(!wrap.step(error));assert(!wrap.step(error)&&wrap.gets==0);
 tick+=499;assert(!wrap.step(error)&&wrap.gets==0);++tick;assert(wrap.step(error)&&wrap.ledger.accepted==300);
}
'''
        compile_run(code)

if __name__=='__main__':
    unittest.main(verbosity=2)
