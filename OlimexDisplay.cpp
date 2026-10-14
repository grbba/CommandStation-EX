/* Optional Olimex MOD-LCD2.8RTP startup/status display. GPLv3. */
#include "OlimexDisplay.h"

#ifdef OLIMEX_LCD
#include <string.h>
#ifdef WIFI_TELNET
#include <stdio.h>
#include "TelnetConsole.h"
#include "CommandDistributor.h"
#endif

OlimexDisplay::OlimexDisplay()
  : _tft(PD2, PC11, PC12, PC10, -1, -1) {
  memset(_text, ' ', sizeof(_text));
  memset(_drawn, ' ', sizeof(_drawn));
  addDisplay(0);
}

void OlimexDisplay::begin() {
  // Reset is handled by the module; UEXT has no LCD MISO connection.
  _tft.begin();
  _tft.setRotation(1);
  _tft.fillScreen(ILI9341_BLACK);
  _tft.setTextSize(2);
  _tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  _tft.setTextWrap(false);
  _started = true;
}

void OlimexDisplay::_setRow(uint8_t row) {
  _row = row;
  _col = 0;
  if (row < ROWS) memset(_text[row], ' ', COLS);
}

size_t OlimexDisplay::_write(uint8_t c) {
  if (_row >= ROWS || _col >= COLS) return 0;
  if (c < ' ' || c > '~') return 0;
  _text[_row][_col++] = c;
  return 1;
}

void OlimexDisplay::_clear() {
  memset(_text, ' ', sizeof(_text));
}

bool OlimexDisplay::drawNextCharacter() {
  if (!_started) return false;
  for (uint16_t count = 0; count < ROWS * COLS; count++) {
    uint16_t position = _scan;
    _scan = (_scan + 1) % (ROWS * COLS);
    uint8_t row = position / COLS;
    uint8_t col = position % COLS;
    if (_text[row][col] == _drawn[row][col]) continue;
    _tft.setCursor(4 + col * 12, 16 + row * 26);
    _tft.write(_text[row][col]);
    _drawn[row][col] = _text[row][col];
    return true;
  }
  return false;
}

void OlimexDisplay::_displayLoop() {
#ifdef WIFI_TELNET
  if (_started && uint32_t(millis() - _connectionUpdateMs) >= 250) {
    _connectionUpdateMs = millis();
    updateConnections();
  }
#endif
  // One changed glyph per main-loop call; unchanged text causes no SPI traffic.
  drawNextCharacter();
}

#ifdef WIFI_TELNET
void OlimexDisplay::updateConnections() {
  unsigned throttle = 0, telnet = 0, dcc = 0, waiting = 0;
  for (byte client = 0; client < MAX_NUM_TCP_CLIENTS; client++) {
    switch (TelnetConsole::connectionState(client)) {
      case TelnetConsole::TELNET: telnet++; break;
      case TelnetConsole::WAITING: waiting++; break;
      case TelnetConsole::APP_CLIENT:
        switch (CommandDistributor::getClientType(client)) {
          case CommandDistributor::WITHROTTLE_TYPE: throttle++; break;
          case CommandDistributor::COMMAND_TYPE:
          case CommandDistributor::WEBSOCKET_TYPE: dcc++; break;
          default: waiting++; break;
        }
        break;
      default: break;
    }
  }
  char line[COLS + 1];
  snprintf(line, sizeof(line), "WT:%u Tel:%u DCC:%u ?:%u", throttle, telnet, dcc, waiting);
  // Update our own row without disturbing the current LCD writer/cursor.
  memset(_text[7], ' ', COLS);
  memcpy(_text[7], line, strlen(line));
}
#endif

void OlimexDisplay::_refresh() {
  // Used only for boot status before the DCC engine starts.
  while (drawNextCharacter()) {}
}
#endif
