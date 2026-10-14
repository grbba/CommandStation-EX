/* Optional Olimex MOD-LCD2.8RTP startup/status display. GPLv3. */
#ifndef OLIMEX_DISPLAY_H
#define OLIMEX_DISPLAY_H

#include "defines.h"

#ifdef OLIMEX_LCD
#if !defined(ARDUINO_NUCLEO_F439ZI) && !defined(ARDUINO_NUCLEO_F4X9ZI)
#error OLIMEX_LCD currently supports the confirmed Nucleo F439ZI wiring only
#endif

#include <Adafruit_ILI9341.h>
#include "DisplayInterface.h"

// MOD-LCD2.8RTP repurposes UEXT MISO (pin 7) as D/C.
// Software SPI preserves the user's CN11 wiring; no touch/I2C driver is used.
class OlimexDisplay : public DisplayInterface {
public:
  OlimexDisplay();
  void begin() override;
  void _setRow(uint8_t row) override;
  size_t _write(uint8_t c) override;
  void _clear() override;
  void _refresh() override;
  void _displayLoop() override;

private:
  static const uint8_t ROWS = 8;
  static const uint8_t COLS = 26;
  Adafruit_ILI9341 _tft;
  char _text[ROWS][COLS];
  char _drawn[ROWS][COLS];
  uint8_t _row = ROWS;
  uint8_t _col = 0;
  uint16_t _scan = 0;
  bool _started = false;
#ifdef WIFI_TELNET
  uint32_t _connectionUpdateMs = 0;
  void updateConnections();
#endif
  bool drawNextCharacter();
};
#endif
#endif
