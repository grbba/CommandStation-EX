// Standalone LCD communication test; excluded from normal firmware builds.
#ifdef OLIMEX_DISPLAY_PROBE
#include <Arduino.h>
#include <Adafruit_ILI9341.h>

// Hardware-confirmed UEXT mapping: 10=CS, 7=D/C, 8=MOSI, 9=SCK.
constexpr int8_t PROBE_CS = PD2;
constexpr int8_t PROBE_CLK = PC10;
Adafruit_ILI9341 probe(PROBE_CS, PC11, PC12, PROBE_CLK, -1, -1);

void setup() {
  Serial.begin(115200);
  // Allow the LCD's own power-on reset circuit to settle after power-up.
  delay(1000);
  Serial.println("Olimex standalone display test: CS=PD2 DC=PC11 MOSI=PC12 CLK=PC10");
  Serial.print("Arduino pin indices: ");
  Serial.print(PROBE_CS); Serial.print(' ');
  Serial.print(PC11); Serial.print(' ');
  Serial.print(PC12); Serial.print(' ');
  Serial.println(PROBE_CLK);
  probe.begin();
  probe.setRotation(1);
  Serial.println("LCD initialization commands sent");
}

void loop() {
  const uint16_t colors[] = {ILI9341_RED, ILI9341_GREEN, ILI9341_BLUE};
  for (uint16_t color : colors) {
    probe.fillScreen(color);
    delay(1000);
  }
  probe.fillScreen(ILI9341_BLACK);
  probe.setTextColor(ILI9341_WHITE);
  probe.setTextSize(2);
  probe.setCursor(12, 24);
  probe.println("Olimex LCD test");
  probe.println("PC11 DC / PC12 MOSI");
  probe.println("PC10 CLK / PD2 CS");
  Serial.println("Color/text cycle sent (write-only: reception unverified)");
  delay(3000);
}
#endif
