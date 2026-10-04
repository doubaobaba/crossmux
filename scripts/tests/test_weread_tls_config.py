"""Guard the existing SDK-owned 2 KiB TLS fragment request (not peer acceptance)."""

import configparser
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


class WeReadTlsConfigTest(unittest.TestCase):
    def test_firmware_keeps_fragment_support_without_sni_wrapper(self):
        ini = (ROOT / "platformio.ini").read_text()
        config = configparser.ConfigParser(interpolation=None)
        config.read_string(ini)
        self.assertIn("-DHAVE_MAX_FRAGMENT", config["base"]["build_flags"].split())
        self.assertNotIn("--wrap=wolfSSL_UseSNI", ini)
        self.assertNotIn("wolfssl_max_fragment.cpp", ini)
        self.assertFalse((ROOT / "src/platform/wolfssl_max_fragment.cpp").exists())

    def test_secure_client_preserves_numeric_failure_after_cleanup(self):
        from test_weread_time_sync import compile_run
        base = ROOT / "freeink-sdk/libs/network/SecureNet"
        header = (base/"include/SecureClient.h").read_text()
        header = re.sub(r'^#(?:include.*|pragma once)\n', '', header, flags=re.M).replace(' override', '')
        source = re.sub(r'^#include.*\n', '', (base/"src/SecureClient.cpp").read_text(), flags=re.M)
        compile_run(r"""
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cassert>
#define FREEINK_NET_WOLFSSL 1
#define WOLFSSL_SUCCESS 1
#define WOLFSSL_VERIFY_NONE 0
#define WOLFSSL_VERIFY_PEER 1
#define WOLFSSL_FILETYPE_PEM 1
#define WOLFSSL_SNI_HOST_NAME 1
#define WOLFSSL_ERROR_WANT_READ 2
#define WOLFSSL_ERROR_WANT_WRITE 3
#define WOLFSSL_ERROR_ZERO_RETURN 4
#define WOLFSSL_CBIO_ERR_CONN_CLOSE -1
#define WOLFSSL_CBIO_ERR_WANT_WRITE -2
#define WOLFSSL_CBIO_ERR_WANT_READ -3
unsigned tick=0,stops=0,frees=0,attempt=0;int fault=0,verified=0;
unsigned millis(){return ++tick;} void delay(unsigned n){tick+=n;}
struct IPAddress { unsigned operator[](int){return 0;} };
struct Client { unsigned getTimeout(){return 10;} };
struct WiFiClient {
 bool alive=false;void setConnectionTimeout(unsigned){}
 bool connect(const char*,uint16_t){return alive=fault!=1;}
 bool connected(){return alive;} void stop(){alive=false;++stops;}
 int available(){return 0;} int write(const uint8_t*,int n){return n;} int read(uint8_t*,int){return 0;}
};
struct { operator bool(){return false;} template<class... T>void printf(const char*,T...){} void println(const char*){} } Serial;
struct { unsigned getFreeHeap(){return 50000;} } ESP;
struct WOLFSSL_METHOD {};struct WOLFSSL_CTX {};struct WOLFSSL {};
WOLFSSL_METHOD method;WOLFSSL_CTX ctx;WOLFSSL ssl;
WOLFSSL_METHOD* wolfSSLv23_client_method(){attempt=0;return &method;}
WOLFSSL_METHOD* wolfTLSv1_2_client_method(){attempt=1;return &method;}
WOLFSSL_CTX* wolfSSL_CTX_new(WOLFSSL_METHOD*){return fault==2?nullptr:&ctx;}
void wolfSSL_CTX_free(WOLFSSL_CTX*){++frees;}void wolfSSL_free(WOLFSSL*){++frees;}
void wolfSSL_CTX_set_verify(WOLFSSL_CTX*,int v,void*){verified=v;}
int wolfSSL_CTX_load_verify_buffer(WOLFSSL_CTX*,const unsigned char*,size_t,int){return fault==3?-155:1;}
void wolfSSL_SetIORecv(WOLFSSL_CTX*,int(*)(WOLFSSL*,char*,int,void*)){}
void wolfSSL_SetIOSend(WOLFSSL_CTX*,int(*)(WOLFSSL*,char*,int,void*)){}
WOLFSSL* wolfSSL_new(WOLFSSL_CTX*){return fault==4?nullptr:&ssl;}
int wolfSSL_check_domain_name(WOLFSSL*,const char*){return fault==5?-170:1;}
void wolfSSL_SetIOReadCtx(WOLFSSL*,void*){}void wolfSSL_SetIOWriteCtx(WOLFSSL*,void*){}
void wolfSSL_UseSNI(WOLFSSL*,int,const char*,size_t){}
int wolfSSL_connect(WOLFSSL*){return fault==6||fault==7?-1:1;}
int wolfSSL_get_error(WOLFSSL*,int){return fault==7?2:(fault==6?(attempt?-188:-125):-125);}
const char* wolfSSL_get_version(WOLFSSL*){return "test";}const char* wolfSSL_get_cipher(WOLFSSL*){return "test";}
int wolfSSL_write(WOLFSSL*,const uint8_t*,size_t n){return fault==8?-1:int(n);}
int wolfSSL_read(WOLFSSL*,uint8_t*,size_t){return -1;}int wolfSSL_pending(WOLFSSL*){return 0;}
""" + header + source + r"""
int main(){
 using C=freeink::SecureClient;using S=C::ErrorStage;
 for(int f=1;f<=7;++f){
  fault=f;C c;c.setCACert("root");assert(!c.connect("official.test",443));
  const S stages[]={S::None,S::Tcp,S::Context,S::Trust,S::Session,S::Hostname,S::Handshake,S::Timeout};
  assert(c.lastErrorStage()==stages[f]);int err=c.lastError();
  c.stop();assert(c.lastErrorStage()==stages[f]&&c.lastError()==err);
  if(f==6){assert(err==-188&&c.firstHandshakeError()==-125);}
  fault=0;assert(c.connect("official.test",443)&&c.lastErrorStage()==S::None&&c.lastError()==0);
  assert(c.firstHandshakeError()==0&&verified==1);
 }
 fault=0;C c;c.setCACert("root");assert(c.connect("official.test",443));
 uint8_t b=0;fault=8;assert(c.write(&b,1)==0&&c.lastErrorStage()==S::Write&&c.lastError()==-125);
 c.stop();fault=0;assert(c.connect("official.test",443));assert(c.read(&b,1)==-1);
 assert(c.lastErrorStage()==S::Read&&c.lastError()==-125);c.stop();assert(c.lastError()==-125);
}
""")

    def test_pinned_sdk_requests_two_kib_once_before_handshake(self):
        path = ROOT / "freeink-sdk/libs/network/SecureNet/src/SecureClient.cpp"
        self.assertTrue(path.is_file(), "Initialize the pinned freeink-sdk submodule before testing")
        source = re.sub(r"//[^\n]*|/\*.*?\*/", "", path.read_text(), flags=re.S)
        call = r"wolfSSL_UseMaxFragment\s*\(\s*\w+\s*,\s*(\w+)\s*\)\s*;"
        self.assertEqual(re.findall(call, source), ["WOLFSSL_MFL_2_11"])
        self.assertRegex(source, r"#\s*ifdef\s+HAVE_MAX_FRAGMENT\s+" + call + r"\s*#\s*endif")
        self.assertLess(re.search(call, source).start(), re.search(r"wolfSSL_connect\s*\(", source).start())


if __name__ == "__main__":
    unittest.main()
