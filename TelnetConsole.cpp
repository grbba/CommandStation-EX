/* Optional shared-port WiFi console. GPLv3. */
#include "TelnetConsole.h"
#ifdef WIFI_TELNET
#include <string.h>
#include <stdio.h>

namespace {
#ifndef WIFI_TELNET_PROBE_MS
#define WIFI_TELNET_PROBE_MS 1000
#endif
constexpr byte NONE = 255;
constexpr unsigned LINE = 128;
constexpr byte OUTPUTS = 8;
constexpr unsigned OUTPUT_SIZE = 320;
constexpr byte COMMANDS = 4;
constexpr byte IAC = 255, DO = 253, DONT = 254, WILL = 251, WONT = 252;
enum Mode : byte { UNDECIDED, APPLICATION, CANDIDATE, CONSOLE };
enum Decoder : byte { TEXT, ESCAPE, OPTION, SUBNEGOTIATION, SUBESCAPE };
struct Session {
  Mode mode = UNDECIDED;
  Decoder decoder = TEXT;
  byte optionCommand = 0;
  bool afterCR = false;
  bool connected = false;
  bool offersStarted = false;
  uint32_t connectedMs = 0;
  bool announced = false;
  bool echoEnabled = false;
  bool offeringEcho = false;
  bool peerSGA = false;
  bool offeringPeerSGA = false;
  byte control[128] = {};
  byte controlLength = 0;
  byte echo[128] = {};
  byte echoLength = 0;
  bool redraw = false;
  bool suppressGoAhead = false;
  bool offeringSGA = false;
  bool discard = false;
  uint16_t length = 0;
  uint16_t generation = 0;
  char line[LINE] = {};
};
struct Output {
  byte client;
  uint16_t generation;
  uint16_t length;
  byte data[OUTPUT_SIZE];
};
struct Command {
  byte client;
  uint16_t generation;
  char text[LINE];
};
Session sessions[MAX_NUM_TCP_CLIENTS];
Output outputs[OUTPUTS];
Command commands[COMMANDS];
byte outputHead = 0, outputCount = 0, commandHead = 0, commandCount = 0;
byte owner = NONE, diagnostics = NONE;
unsigned suppressed = 0;
uint32_t dropped = 0;

bool enqueue(byte client, const byte *data, unsigned length) {
  if (client >= MAX_NUM_TCP_CLIENTS || length > OUTPUT_SIZE || outputCount == OUTPUTS)
    return false;
  Output &out = outputs[(outputHead + outputCount) % OUTPUTS];
  out.client = client;
  out.generation = sessions[client].generation;
  out.length = length;
  memcpy(out.data, data, length);
  outputCount++;
  return true;
}
bool flushControl(byte client) {
  Session &s = sessions[client];
  if (!s.controlLength) return true;
  if (!enqueue(client, s.control, s.controlLength)) return false;
  s.controlLength = 0;
  return true;
}
bool option(byte client, byte verb, byte code) {
  Session &s = sessions[client];
  if (s.controlLength + 3 > sizeof(s.control) && !flushControl(client)) return false;
  s.control[s.controlLength++] = IAC;
  s.control[s.controlLength++] = verb;
  s.control[s.controlLength++] = code;
  return true;
}
bool flushEcho(byte client) {
  Session &s = sessions[client];
  if (!flushControl(client)) return false;
  if (s.redraw) {
    byte data[LINE + 16];
    const char *prefix = s.mode == CONSOLE ? "\r\033[Kdcc> " : "\r\033[K";
    unsigned length = strlen(prefix);
    memcpy(data, prefix, length);
    memcpy(data + length, s.line, s.length);
    if (!enqueue(client, data, length + s.length)) return false;
    s.redraw = false;
    s.echoLength = 0;
  } else if (s.echoLength) {
    if (!enqueue(client, s.echo, s.echoLength)) return false;
    s.echoLength = 0;
  }
  return true;
}
void echo(byte client, const char *data, unsigned length) {
  Session &s = sessions[client];
  if (!s.echoEnabled || s.redraw) return;
  if (s.echoLength + length > sizeof(s.echo) && !flushEcho(client)) {
    s.echoLength = 0;
    s.redraw = true;
    return;
  }
  memcpy(s.echo + s.echoLength, data, length);
  s.echoLength += length;
}
void message(byte client, const char *text) {
  if (flushEcho(client)) enqueue(client, (const byte *)text, strlen(text));
}
void prompt(byte client) { message(client, "dcc> "); }
void offerOptions(byte client) {
  Session &s = sessions[client];
  if (s.offersStarted) return;
  s.offersStarted = true;
  s.offeringEcho = option(client, WILL, 1);
  s.offeringSGA = option(client, WILL, 3);
  s.offeringPeerSGA = option(client, DO, 3);
}
void announce(byte client) {
  Session &s = sessions[client];
  if (s.announced || s.length) return;
  s.announced = true;
  offerOptions(client);
  message(client, "DCC-EX: enter #console then Enter.\r\n");
}

class Diagnostic : public Print {
public:
  byte data[OUTPUT_SIZE];
  unsigned length = 0;
  bool overflow = false;
  size_t write(uint8_t value) override {
    // Keep room for message terminator, and escape Telnet's command byte.
    unsigned need = value == IAC ? 2 : 1;
    if (length + need > 160) { overflow = true; return 0; }
    data[length++] = value;
    if (value == IAC) data[length++] = value;
    return 1;
  }
} diagnostic;
bool capturing = false;

void lineReady(byte client) {
  Session &s = sessions[client];
  s.line[s.length] = 0;
  if (s.discard) {
    message(client, "\r\nLine too long; discarded.\r\n");
    if (s.mode == CONSOLE) prompt(client);
  } else if (s.mode == CANDIDATE) {
    if (!strcmp(s.line, "#console")) {
      if (owner != NONE && owner != client) {
        message(client, "Console already in use.\r\n");
      } else {
        owner = client;
        s.mode = CONSOLE;
        message(client, "\r\nDCC-EX console. Raw <commands>; help; diag on/off.\r\n");
        prompt(client);
      }
    } else if (s.length) message(client, "Enter #console to open the console.\r\n");
  } else if (s.length) {
    if (!strcmp(s.line, "help")) {
      message(client, "\r\nUse <s> for status, DCC <commands>, diag on, diag off.\r\n");
      prompt(client);
    } else if (!strcmp(s.line, "diag on")) {
      diagnostics = client;
      message(client, "\r\nDiagnostics on. WiFi transport traces remain serial-only.\r\n");
      prompt(client);
    } else if (!strcmp(s.line, "diag off")) {
      diagnostics = NONE;
      message(client, "\r\nDiagnostics off.\r\n");
      prompt(client);
    } else if (s.line[0] == '<' && s.line[s.length - 1] == '>') {
      if (commandCount == COMMANDS) {
        message(client, "\r\nCommand queue full; retry.\r\n");
        prompt(client);
      } else {
        Command &command = commands[(commandHead + commandCount) % COMMANDS];
        command.client = client;
        command.generation = s.generation;
        memcpy(command.text, s.line, s.length + 1);
        commandCount++;
        // Prompt is emitted after the command has been handed to the core.
      }
    } else {
      message(client, "\r\nExpected a complete <command>, help, or diag on/off.\r\n");
      prompt(client);
    }
  } else if (s.mode == CONSOLE) prompt(client);
  s.length = 0;
  s.discard = false;
}
}

void TelnetConsole::connected(byte client) {
  if (client >= MAX_NUM_TCP_CLIENTS) return;
  sessions[client].connected = true;
  sessions[client].connectedMs = millis();
}
bool TelnetConsole::handles(byte client) {
  return client < MAX_NUM_TCP_CLIENTS &&
    (sessions[client].mode == CANDIDATE || sessions[client].mode == CONSOLE);
}
bool TelnetConsole::claim(byte client, byte first) {
  if (client >= MAX_NUM_TCP_CLIENTS) return false;
  Session &s = sessions[client];
  if (s.mode == UNDECIDED) {
    s.mode = (first == '#' || first == IAC) ? CANDIDATE : APPLICATION;
    if (s.mode == CANDIDATE) {
      offerOptions(client);
      // A literal #console is already an entry request, so do not instruct
      // that caller to enter it again. Telnet negotiators get an entry hint.
      if (first == IAC) announce(client);
    }
  }
  return handles(client);
}
bool TelnetConsole::isConsole(byte client) {
  client &= CLIENT_MASK;
  return client < MAX_NUM_TCP_CLIENTS && sessions[client].mode == CONSOLE;
}
void TelnetConsole::receive(byte client, byte value) {
  if (!handles(client)) return;
  Session &s = sessions[client];
  switch (s.decoder) {
    case ESCAPE:
      if (value == IAC) { s.decoder = TEXT; value = IAC; break; }
      if (value == DO || value == DONT || value == WILL || value == WONT) {
        s.optionCommand = value; s.decoder = OPTION; return;
      }
      s.decoder = value == 250 ? SUBNEGOTIATION : TEXT;
      return;
    case OPTION: {
      if (value == 1 && s.optionCommand == DO) {
        if (s.offeringEcho) s.offeringEcho = false;
        else if (!s.echoEnabled) option(client, WILL, 1);
        s.echoEnabled = true;
      } else if (value == 1 && s.optionCommand == DONT) {
        if (s.echoEnabled && !s.offeringEcho) option(client, WONT, 1);
        s.offeringEcho = false;
        s.echoEnabled = false;
      } else if (value == 3 && s.optionCommand == DO) {
        if (s.offeringSGA) s.offeringSGA = false;
        else if (!s.suppressGoAhead) option(client, WILL, 3);
        s.suppressGoAhead = true;
      } else if (value == 3 && s.optionCommand == DONT) {
        if (s.suppressGoAhead && !s.offeringSGA) option(client, WONT, 3);
        s.offeringSGA = false;
        s.suppressGoAhead = false;
      } else if (value == 3 && s.optionCommand == WILL) {
        if (s.offeringPeerSGA) s.offeringPeerSGA = false;
        else if (!s.peerSGA) option(client, DO, 3);
        s.peerSGA = true;
      } else if (value == 3 && s.optionCommand == WONT) {
        if (s.peerSGA && !s.offeringPeerSGA) option(client, DONT, 3);
        s.offeringPeerSGA = false;
        s.peerSGA = false;
      } else if (s.optionCommand == DO) option(client, WONT, value);
      else if (s.optionCommand == WILL) option(client, DONT, value);
      // Negative acknowledgements and duplicate enabled options are silent.
      s.decoder = TEXT; return;
    }
    case SUBNEGOTIATION: if (value == IAC) s.decoder = SUBESCAPE; return;
    case SUBESCAPE: s.decoder = value == 240 ? TEXT : SUBNEGOTIATION; return;
    case TEXT: if (value == IAC) { s.decoder = ESCAPE; return; } break;
  }
  if (s.afterCR) {
    s.afterCR = false;
    if (value == '\n' || value == 0) return;
  }
  if (value == '\r' || value == '\n') {
    s.afterCR = value == '\r';
    echo(client, "\r\n", 2);
    lineReady(client); return;
  }
  if (value == 8 || value == 127) {
    if (!s.discard && s.length) { s.length--; echo(client, "\b \b", 3); }
    return;
  }
  if (value < 32 || value > 126 || s.discard) return;
  if (s.length == LINE - 1) { s.discard = true; return; }
  s.line[s.length++] = value;
  char character = value;
  echo(client, &character, 1);
}
uint16_t TelnetConsole::generation(byte client) {
  client &= CLIENT_MASK;
  return client < MAX_NUM_TCP_CLIENTS ? sessions[client].generation : 0;
}
void TelnetConsole::forget(byte client) {
  if (client >= MAX_NUM_TCP_CLIENTS) return;
  uint16_t generation = sessions[client].generation + 1;
  sessions[client] = Session();
  sessions[client].generation = generation;
  if (owner == client) owner = NONE;
  if (diagnostics == client) diagnostics = NONE;
}
bool TelnetConsole::nextCommand(byte &client, byte *command) {
  while (commandCount) {
    Command &entry = commands[commandHead];
    commandHead = (commandHead + 1) % COMMANDS; commandCount--;
    if (sessions[entry.client].generation != entry.generation) continue;
    client = entry.client;
    memcpy(command, entry.text, LINE);
    prompt(client);
    return true;
  }
  return false;
}
void TelnetConsole::pump(RingStream *ring) {
  // Coalesce negotiation replies and keystroke echo before using CIPSEND.
  // A client option flurry therefore does not consume one queue slot per option.
  for (byte client = 0; client < MAX_NUM_TCP_CLIENTS; client++) {
    Session &s = sessions[client];
    // A server-first terminal otherwise waits forever for our negotiation.
    // Only control bytes are probed; classify on its reply before sending text.
    if (WIFI_TELNET_PROBE_MS && s.connected && s.mode == UNDECIDED &&
        !s.offersStarted && uint32_t(millis() - s.connectedMs) >= WIFI_TELNET_PROBE_MS)
      offerOptions(client);
    if (handles(client) || (s.mode == UNDECIDED && s.offersStarted)) flushEcho(client);
  }
  // Only inject when the shared output ring and CIPSEND are idle. The caller
  // immediately claims this frame, allowing close/error handling to purge it.
  while (outputCount) {
    Output &entry = outputs[outputHead];
    if (sessions[entry.client].generation != entry.generation) {
      outputHead = (outputHead + 1) % OUTPUTS; outputCount--; continue;
    }
    if (ring->freeSpace() < entry.length + 3 || ring->peekTargetMark() != RingStream::NO_CLIENT)
      return;
    ring->mark(entry.client | CLIENT_MARKER);
    ring->write(entry.data, entry.length);
    if (!ring->commit()) return;
    outputHead = (outputHead + 1) % OUTPUTS; outputCount--;
    return;
  }
  if (diagnostics != NONE && dropped) {
    char text[80];
    snprintf(text, sizeof(text), "\r\n<* Dropped %lu diagnostic messages *>\r\n", (unsigned long)dropped);
    if (enqueue(diagnostics, (const byte *)text, strlen(text))) dropped = 0;
  }
}
Print *TelnetConsole::beginDiagnostic() {
  if (diagnostics == NONE || suppressed || capturing) return nullptr;
  capturing = true; diagnostic.length = 0; diagnostic.overflow = false;
  diagnostic.write('\r'); diagnostic.write('\n');
  diagnostic.write('<'); diagnostic.write('*'); diagnostic.write(' ');
  return &diagnostic;
}
void TelnetConsole::endDiagnostic() {
  if (!capturing) return;
  capturing = false;
  const byte end[] = {' ', '*', '>', '\r', '\n'};
  memcpy(diagnostic.data + diagnostic.length, end, sizeof(end));
  diagnostic.length += sizeof(end);
  // A diagnostic starts on a fresh line and restores the local-echo input.
  const char label[] = "dcc> ";
  memcpy(diagnostic.data + diagnostic.length, label, sizeof(label) - 1);
  diagnostic.length += sizeof(label) - 1;
  Session &session = sessions[diagnostics];
  memcpy(diagnostic.data + diagnostic.length, session.line, session.length);
  diagnostic.length += session.length;
  if (diagnostic.overflow || outputCount >= OUTPUTS - 3 ||
      !enqueue(diagnostics, diagnostic.data, diagnostic.length)) dropped++;
}
TelnetConsole::MuteDiagnostics::MuteDiagnostics() { suppressed++; }
TelnetConsole::MuteDiagnostics::~MuteDiagnostics() { suppressed--; }
#endif
