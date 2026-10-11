/* Optional shared-port WiFi console. GPLv3. */
#include "TelnetConsole.h"
#ifdef WIFI_TELNET
#include <string.h>
#include <stdio.h>

namespace {
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
void message(byte client, const char *text) {
  enqueue(client, (const byte *)text, strlen(text));
}
void prompt(byte client) { message(client, "dcc> "); }

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
        // Client supplies local echo; server supports suppress-go-ahead.
        if (!s.suppressGoAhead && !s.offeringSGA) {
          const byte negotiation[] = {IAC, WILL, 3};
          if (enqueue(client, negotiation, sizeof(negotiation))) s.offeringSGA = true;
        }
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

bool TelnetConsole::handles(byte client) {
  return client < MAX_NUM_TCP_CLIENTS &&
    (sessions[client].mode == CANDIDATE || sessions[client].mode == CONSOLE);
}
bool TelnetConsole::claim(byte client, byte first) {
  if (client >= MAX_NUM_TCP_CLIENTS) return false;
  Session &s = sessions[client];
  if (s.mode == UNDECIDED) s.mode = (first == '#' || first == IAC) ? CANDIDATE : APPLICATION;
  return handles(client);
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
      byte response = 0;
      if (s.optionCommand == DO && value == 3) {
        if (s.offeringSGA) s.offeringSGA = false; // acknowledge our offer
        else if (!s.suppressGoAhead) response = WILL;
        s.suppressGoAhead = true;
      } else if (s.optionCommand == DONT && value == 3) {
        if (s.suppressGoAhead && !s.offeringSGA) response = WONT;
        s.offeringSGA = false;
        s.suppressGoAhead = false;
      } else if (s.optionCommand == DO) response = WONT;
      else if (s.optionCommand == WILL) response = DONT;
      // Duplicate enabled options and negative acknowledgements get no reply.
      if (response) {
        byte bytes[] = {IAC, response, value};
        enqueue(client, bytes, sizeof(bytes));
      }
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
    s.afterCR = value == '\r'; lineReady(client); return;
  }
  if (value == 8 || value == 127) { if (!s.discard && s.length) s.length--; return; }
  if (value < 32 || value > 126 || s.discard) return;
  if (s.length == LINE - 1) { s.discard = true; return; }
  s.line[s.length++] = value;
}
uint16_t TelnetConsole::generation(byte client) {
  client &= 0x3f;
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
