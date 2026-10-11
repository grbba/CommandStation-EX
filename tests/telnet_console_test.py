#!/usr/bin/env python3
"""Test the real console, AT input handler and RingStream with fake UART/core."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="dccex-telnet-") as directory:
    work = Path(directory)
    for name in ("TelnetConsole.h", "TelnetConsole.cpp", "WifiInboundHandler.h",
                 "WifiInboundHandler.cpp", "RingStream.h", "RingStream.cpp"):
        (work / name).write_text((ROOT / name).read_text())
    (work / "defines.h").write_text('#pragma once\n#define WIFI_TELNET\n#define MAX_NUM_TCP_CLIENTS 9\n')
    (work / "Arduino.h").write_text(r'''#pragma once
#include "defines.h"
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <deque>
#include <vector>
using byte=uint8_t;
#define F(x) (x)
inline byte highByte(int n){return n>>8;}
inline byte lowByte(int n){return n;}
inline uint32_t now=1000;
inline uint32_t millis(){return now;}
class Print {
public:
 virtual ~Print(){};
 virtual size_t write(uint8_t)=0;
 virtual int availableForWrite(){return 0;}
 size_t write(const uint8_t* p,size_t n){size_t r=0;while(n--)r+=write(*p++);return r;}
 size_t print(const char* p){return write((const byte*)p,strlen(p));}
};
class Stream: public Print {
public: std::deque<byte> input; std::string output;
 int available(){return input.size();}
 int read(){if(input.empty())return -1;byte c=input.front();input.pop_front();return c;}
 size_t write(byte c) override {output+=(char)c;return 1;}
 void feed(const std::string& s){for(unsigned char c:s)input.push_back(c);}
};
''')
    (work / "FSH.h").write_text('#pragma once\nusing FSH=char;\n#define STRLEN_P strlen\n#define GETFLASH(x) (*(x))\n')
    (work / "DIAG.h").write_text(r'''#pragma once
#include "Arduino.h"
#define DIAG(...)
class Diag {public: static inline bool WIFI=false;static inline bool WEBSOCKET=false;};
class StringFormatter {public:
 static void printEscape(int){}
 template<class... Args> static void send(Print* p,const char* f,Args... args){
   char b[100];snprintf(b,sizeof(b),f,args...);p->print(b);
 }
};
''')
    (work / "WiThrottle.h").write_text('#pragma once\n#include "RingStream.h"\nclass WiThrottle {public: static inline unsigned loops=0;static void loop(RingStream*){loops++;}};\n')
    (work / "Websockets.h").write_text('#pragma once\n#include "Arduino.h"\nclass Websockets {public: static constexpr byte WEBSOCK_CLIENT_MARKER=128;static int getOutboundHeaderSize(int){return 0;}static void writeOutboundHeader(Stream*,int){}};\n')
    (work / "CommandDistributor.h").write_text(r'''#pragma once
#include "TelnetConsole.h"
class CommandDistributor {public:
 static inline std::vector<std::pair<byte,std::string>> calls;
 static void parse(byte id,byte* input,RingStream* ring){
  calls.emplace_back(id,(char*)input);
  ring->mark(id);ring->print("reply\r\n");ring->commit();
 }
 static void forget(byte id){TelnetConsole::forget(id);}
};
''')
    parser = (ROOT / "DCCEXParser.cpp").read_text()
    callbacks = parser[parser.index("bool DCCEXParser::stashCallback("):parser.index("void DCCEXParser::callback_W(")]
    (work / "test.cpp").write_text(r'''#include "WifiInboundHandler.h"
#include "TelnetConsole.h"
#include "CommandDistributor.h"
#include <cassert>
#include <iostream>
#define MAX_COMMAND_PARAMS 4
class DCCEXParser {
public:
 static inline bool stashBusy=false;
 static inline Print* stashStream=nullptr;
 static inline RingStream* stashRingStream=nullptr;
 static inline byte stashTarget=0;
 static inline uint16_t stashGeneration=0;
 static inline int16_t stashP[MAX_COMMAND_PARAMS]={};
 static bool stashCallback(Print*,int16_t[],RingStream*);
 static Print* getAsyncReplyStream();
 static void commitAsyncReplyStream();
};
CALLBACK_IMPLEMENTATIONS
Stream uart;
size_t acknowledged=0;
bool acknowledge=true;
void cycles(int n=50){
 for(int i=0;i<n;i++){
  now+=10;WifiInboundHandler::loop();
  size_t found=uart.output.find("AT+CIPSEND=",acknowledged);
  if(acknowledge && found!=std::string::npos){
   size_t end=uart.output.find("\r\n",found);
   if(end!=std::string::npos){acknowledged=end+2;uart.feed(">\r\nSEND OK\r\n");}
  }
 }
}
void packet(byte id,const std::string& text){
 uart.feed("+IPD,"+std::to_string(id)+","+std::to_string(text.size())+":"+text);
 cycles();
}
void close(byte id){uart.feed(std::to_string(id)+",CLOSED\r\n");cycles();}
void enter(byte id){packet(id,"#con");packet(id,"sole\r");packet(id,"\n");}
void diagnostic(const char* text){Print* sink=TelnetConsole::beginDiagnostic();if(sink){sink->print(text);TelnetConsole::endDiagnostic();}}
int main(){
 WifiInboundHandler::setup(&uart);
 // A normal throttle passes through unchanged and receives no console greeting.
 packet(1,"NThrottle\n");
 assert(CommandDistributor::calls.back()==std::make_pair((byte)1,std::string("NThrottle\n")));
 assert(uart.output.find("DCC-EX console")==std::string::npos);
 // Byte-wise option negotiation must survive the real ring's 0xff marker handling.
 packet(0,std::string(1,(char)255));packet(0,std::string(1,(char)253));packet(0,std::string(1,(char)42));
 assert(uart.output.find(std::string("\xff\xfc\x2a",3))!=std::string::npos);
 enter(0);assert(uart.output.find("DCC-EX console")!=std::string::npos);
 // CRLF and CR-NUL normalized; fragmented commands preserved.
 size_t before=CommandDistributor::calls.size();
 packet(0,"<");packet(0,"s>");packet(0,"\r");packet(0,std::string("\0",1));
 assert(CommandDistributor::calls.size()==before+1);
 assert(CommandDistributor::calls.back()==std::make_pair((byte)0,std::string("<s>")));
 // Interleaved throttle continues to operate with console active.
 packet(1,"M0A\n");assert(CommandDistributor::calls.back().first==1);
 // Backspace/delete edit the buffered line.
 packet(0,std::string("<sx\b>\n"));assert(CommandDistributor::calls.back().second=="<s>");
 // Large input takes console path, not websocket prescan, and is rejected safely.
 before=CommandDistributor::calls.size();packet(0,"<"+std::string(300,'x')+">\n");
 assert(CommandDistributor::calls.size()==before);
 assert(uart.output.find("Line too long")!=std::string::npos);
 packet(0,"<s>\n");assert(CommandDistributor::calls.size()==before+1);
 // Long WebSocket handshakes retain the existing key-only prescan behavior.
 packet(3,"GET / HTTP/1.1\r\n"+std::string(120,'x')+"\r\nSec-WebSocket-Key: abc\r\n\r\n");
 assert(CommandDistributor::calls.back()==std::make_pair((byte)3,std::string("Sec-WebSocket-Key: abc\r\n")));
 // Diagnostics are subscription-based and suppression prevents transport feedback.
 size_t start=uart.output.size();diagnostic("hidden");cycles();
 assert(uart.output.substr(start).find("hidden")==std::string::npos);
 packet(0,"diag on\n");diagnostic("visible");cycles();
 assert(uart.output.find("<* visible *>\r\n")!=std::string::npos);
 start=uart.output.size();{TelnetConsole::MuteDiagnostics mute;diagnostic("muted");}cycles();
 assert(uart.output.substr(start).find("muted")==std::string::npos);
 // Restore partially typed local-echo input after an asynchronous diagnostic.
 packet(0,"<s");diagnostic("while typing");cycles();
 assert(uart.output.find("while typing *>\r\ndcc> <s")!=std::string::npos);
 packet(0,">\n");
 // Queue saturation drops diagnostics but allows control and reports loss.
 for(int n=0;n<100;n++)diagnostic("burst");cycles(300);
 assert(uart.output.find("Dropped ")!=std::string::npos);
 // Close clears subscription and line state; ordinary throttle slot remains active.
 packet(0,"<unfinished");close(0);before=CommandDistributor::calls.size();
 enter(0);packet(0,"<s>\n");assert(CommandDistributor::calls.size()==before+1);
 start=uart.output.size();diagnostic("stale");cycles();
 assert(uart.output.substr(start).find("stale")==std::string::npos);
 packet(1,"M0B\n");assert(CommandDistributor::calls.back().first==1);
 // Commands/output waiting in console queues cannot leak to a reused ID.
 TelnetConsole::receive(0,'<');TelnetConsole::receive(0,'s');TelnetConsole::receive(0,'>');TelnetConsole::receive(0,'\n');
 TelnetConsole::forget(0);byte id;byte command[128];assert(!TelnetConsole::nextCommand(id,command));
 assert(!TelnetConsole::claim(255,'#'));
 // A second console cannot steal the active console owner.
 enter(0);enter(2);assert(uart.output.find("Console already in use")!=std::string::npos);
 // Subnegotiation and IAC-IAC never reach the DCC parser.
 packet(0,std::string("\xff\xfa",2)+"abc"+std::string("\xff\xf0",2));
 packet(0,"<s>\n");assert(CommandDistributor::calls.back().second=="<s>");
 // Negative negotiation replies must not generate reply loops.
 start=uart.output.size();packet(0,std::string("\xff\xfe\x03",3));
 assert(uart.output.size()==start);
 // Close while literal-IAC output is awaiting CIPSEND must purge safely.
 acknowledge=false;packet(0,"help\n");
 close(0);acknowledged=uart.output.size();acknowledge=true;cycles();
 enter(0);packet(0,"<s>\n");assert(CommandDistributor::calls.back().second=="<s>");
 // Queue cleanup preserves another client's in-flight payload and survivors.
 RingStream cleanup(128);
 cleanup.mark(1);cleanup.print("a");cleanup.commit();
 assert(cleanup.read()==1);assert(cleanup.count()==1);
 cleanup.mark(0);cleanup.print("old");cleanup.commit();
 cleanup.mark(2);cleanup.print("two");cleanup.commit();
 cleanup.discardClient(0,1);
 assert(cleanup.read()=='a');assert(cleanup.read()==2);assert(cleanup.count()==3);
 assert(cleanup.read()=='t');assert(cleanup.read()=='w');assert(cleanup.read()=='o');assert(cleanup.read()==-1);
 // Exercise wraparound while compacting committed frames.
 RingStream wrapped(64);
 wrapped.mark(1);wrapped.print(std::string(40,'x').c_str());wrapped.commit();
 assert(wrapped.read()==1);assert(wrapped.count()==40);for(int n=0;n<40;n++)wrapped.read();
 wrapped.mark(0);wrapped.print("discarded");wrapped.commit();
 wrapped.mark(2);wrapped.print("keep");wrapped.commit();wrapped.discardClient(0,0);
 assert(wrapped.read()==2);assert(wrapped.count()==4);
 for(char c:std::string("keep"))assert(wrapped.read()==c);assert(wrapped.read()==-1);
 // Real programming callback stash functions reject output after ID reuse.
 RingStream async(128);int16_t params[MAX_COMMAND_PARAMS]={};
 async.mark(0);assert(DCCEXParser::stashCallback(&async,params,&async));async.commit();
 assert(DCCEXParser::getAsyncReplyStream()==&async);async.print("valid");DCCEXParser::commitAsyncReplyStream();
 assert(async.read()==0);assert(async.count()==5);for(int n=0;n<5;n++)async.read();
 async.mark(0);assert(DCCEXParser::stashCallback(&async,params,&async));async.commit();
 TelnetConsole::forget(0);
 assert(DCCEXParser::getAsyncReplyStream()==nullptr);DCCEXParser::commitAsyncReplyStream();
 assert(!DCCEXParser::stashBusy);assert(async.read()==-1);
 assert(WiThrottle::loops>0);
 std::cout<<"Telnet, AT framing, raw ring output and mixed-session tests passed.\n";
}
'''.replace('CALLBACK_IMPLEMENTATIONS',callbacks))
    executable = work / "test"
    subprocess.run(["clang++", "-std=c++17", "-fsanitize=address,undefined", "-g",
                    "-I", str(work), *(str(work / name) for name in
                    ("TelnetConsole.cpp", "WifiInboundHandler.cpp", "RingStream.cpp", "test.cpp")),
                    "-o", str(executable)], check=True)
    # Firmware owns its rings for process lifetime; focus on invalid accesses.
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
    subprocess.run([str(executable)], check=True, env=env)
