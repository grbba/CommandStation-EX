/* Optional shared-port WiFi console. GPLv3. */
#ifndef TELNET_CONSOLE_H
#define TELNET_CONSOLE_H
#include "defines.h"
#ifdef WIFI_TELNET
#include "RingStream.h"
class TelnetConsole {
public:
  static constexpr byte CLIENT_MARKER = 0x40;
  static constexpr byte TEXT_MARKER = 0x20;
  static constexpr byte CLIENT_MASK = 0x1f;
  static bool claim(byte client, byte first);
  static bool handles(byte client);
  static bool isConsole(byte client);
  static void receive(byte client, byte value);
  static void forget(byte client);
  static uint16_t generation(byte client);
  static bool nextCommand(byte &client, byte *command);
  static void pump(RingStream *ring);
  static Print *beginDiagnostic();
  static void endDiagnostic();
  class MuteDiagnostics {
  public:
    MuteDiagnostics();
    ~MuteDiagnostics();
  };
};
#endif
#endif
