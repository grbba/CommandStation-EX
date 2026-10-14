#!/usr/bin/env python3
"""Exercise the real status adapter with a fake LCD, under host sanitizers."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="olimex-display-test-") as directory:
    work = Path(directory)
    for name in ("OlimexDisplay.h", "OlimexDisplay.cpp", "DisplayInterface.h"):
        (work / name).write_text((ROOT / name).read_text())
    (work / "defines.h").write_text('''#pragma once
#define OLIMEX_LCD
#define ARDUINO_NUCLEO_F439ZI
#define WIFI_TELNET
#define MAX_NUM_TCP_CLIENTS 5
#define PC10 42
#define PC11 43
#define PC12 44
#define PD2 56
''')
    (work / "Arduino.h").write_text('''#pragma once
#include <cstdint>
#include <cstddef>
using byte=uint8_t;
inline uint32_t now=0;
inline uint32_t millis(){return now;}
class Print { public: virtual ~Print() {} virtual size_t write(uint8_t)=0; };
''')
    (work / "TelnetConsole.h").write_text('''#pragma once
#include "Arduino.h"
class TelnetConsole { public:
  enum ConnectionState : byte { DISCONNECTED, WAITING, TELNET, APP_CLIENT };
  static inline ConnectionState states[5]={};
  static ConnectionState connectionState(byte c){return states[c];}
};
''')
    (work / "CommandDistributor.h").write_text('''#pragma once
#include "Arduino.h"
class CommandDistributor { public:
  enum clientType : byte {NONE_TYPE,COMMAND_TYPE,WITHROTTLE_TYPE,WEBSOCK_CONNECTING_TYPE,WEBSOCKET_TYPE};
  static inline clientType types[5]={};
  static clientType getClientType(byte c){return types[c];}
};
''')
    (work / "Adafruit_ILI9341.h").write_text('''#pragma once
#include "Arduino.h"
#include <cassert>
#include <cstring>
#define ILI9341_BLACK 0
#define ILI9341_WHITE 65535
class Adafruit_ILI9341 {
public:
  static inline unsigned writes=0;
  static inline char screen[8][26];
  int row=0, col=0;
  Adafruit_ILI9341(int cs,int dc,int mosi,int clk,int rst,int miso) {
    assert(cs==PD2 && dc==PC11 && mosi==PC12 && clk==PC10);
    assert(rst==-1 && miso==-1);
  }
  void begin() {}
  void setRotation(int r) {assert(r==1);}
  void fillScreen(int) {memset(screen,' ',sizeof(screen));}
  void setTextSize(int s) {assert(s==2);}
  void setTextColor(int,int) {}
  void setTextWrap(bool w) {assert(!w);}
  void setCursor(int x,int y) {
    assert(x>=4 && x<320 && y>=16 && y+16<=240);
    col=(x-4)/12; row=(y-16)/26;
    assert(row<8 && col<26);
  }
  void write(char c) { screen[row][col]=c; writes++; }
};
''')
    (work / "test.cpp").write_text('''#include "OlimexDisplay.h"
#include "TelnetConsole.h"
#include "CommandDistributor.h"
#include <cassert>
#include <cstring>
DisplayInterface* DisplayInterface::_displayHandler=nullptr;
uint8_t DisplayInterface::_selectedDisplayNo=255;
int main() {
  OlimexDisplay display;
  display._setRow(4);
  const char* ip="192.168.100.123";
  for(const char* p=ip;*p;p++) assert(display._write(*p)==1);
  display._displayLoop();
  assert(Adafruit_ILI9341::writes==0); // safe before initialization
  display.begin();
  unsigned before=Adafruit_ILI9341::writes;
  display._displayLoop();
  assert(Adafruit_ILI9341::writes==before+1); // bounded work per loop
  display._refresh();
  assert(memcmp(Adafruit_ILI9341::screen[4],ip,strlen(ip))==0);
  before=Adafruit_ILI9341::writes;
  for(int n=0;n<100;n++) display._displayLoop();
  assert(Adafruit_ILI9341::writes==before); // no unchanged redraws
  display._setRow(4); display._write('X'); display._refresh();
  assert(Adafruit_ILI9341::screen[4][0]=='X');
  for(int n=1;n<26;n++) assert(Adafruit_ILI9341::screen[4][n]==' ');
  display._setRow(255); assert(display._write('X')==0);
  display._setRow(7);
  for(int n=0;n<26;n++) assert(display._write('A')==1);
  assert(display._write('B')==0);
  display._refresh();
  for(int n=0;n<26;n++) assert(Adafruit_ILI9341::screen[7][n]=='A');
  display._clear(); display._refresh();
  for(auto& row:Adafruit_ILI9341::screen)
    for(char c:row) assert(c==' ');
  TelnetConsole::states[0]=TelnetConsole::APP_CLIENT;
  CommandDistributor::types[0]=CommandDistributor::WITHROTTLE_TYPE;
  TelnetConsole::states[1]=TelnetConsole::TELNET;
  TelnetConsole::states[2]=TelnetConsole::APP_CLIENT;
  CommandDistributor::types[2]=CommandDistributor::WEBSOCKET_TYPE;
  TelnetConsole::states[3]=TelnetConsole::WAITING;
  TelnetConsole::states[4]=TelnetConsole::APP_CLIENT;
  CommandDistributor::types[4]=CommandDistributor::WEBSOCK_CONNECTING_TYPE;
  display._setRow(4); display._write('I');
  now=250; before=Adafruit_ILI9341::writes;
  display._displayLoop();
  assert(Adafruit_ILI9341::writes<=before+1);
  display._write('P'); // status must not change the active LCD cursor
  display._refresh();
  assert(memcmp(Adafruit_ILI9341::screen[4],"IP",2)==0);
  const char* summary="WT:1 Tel:1 DCC:1 ?:2";
  assert(memcmp(Adafruit_ILI9341::screen[7],summary,strlen(summary))==0);
  for(auto& state:TelnetConsole::states) state=TelnetConsole::DISCONNECTED;
  now=500; display._displayLoop(); display._refresh();
  summary="WT:0 Tel:0 DCC:0 ?:0";
  assert(memcmp(Adafruit_ILI9341::screen[7],summary,strlen(summary))==0);
  for(unsigned n=strlen(summary);n<26;n++) assert(Adafruit_ILI9341::screen[7][n]==' ');
  before=Adafruit_ILI9341::writes;
  now=750; display._displayLoop(); display._refresh();
  assert(Adafruit_ILI9341::writes==before);
}
''')
    executable = work / "display-test"
    subprocess.run(["clang++", "-std=c++17", "-fsanitize=address,undefined",
                    "-g", "-I", str(work), str(work / "OlimexDisplay.cpp"),
                    str(work / "test.cpp"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print("Olimex display adapter tests passed (AddressSanitizer/UBSan).")
