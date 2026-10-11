# Olimex startup display on Nucleo F439ZI

This optional build displays firmware/board identity, WiFi setup status, the assigned IP address, port, and the existing Ready message on an Olimex MOD-LCD2.8RTP Rev B. It uses the existing `LCD()` output API, retains each logical row, and does not scroll the address off screen. Telnet support is a separate pending change; showing port 2560 does not imply Telnet negotiation has been implemented.

## Wiring

| UEXT | Module signal | F439ZI |
| --- | --- | --- |
| 1 | 3V3 | CN11 3V3 |
| 2 | GND | CN11 GND |
| 5 | SCL (touch) | PB8, CN12 |
| 6 | SDA (touch) | PB9, CN12 |
| 7 | D/C | PC11, CN11 |
| 8 | MOSI | PC12, CN11 |
| 9 | Clock | PD2, CN11 |
| 10 | Chip select | PC10, CN11 |

[Olimex's module documentation](https://github.com/OLIMEX/MOD-LCD2.8RTP/blob/master/README.md) explains that UEXT pin 7 is D/C, replacing standard UEXT MISO. The [Rev B schematic](https://github.com/OLIMEX/MOD-LCD2.8RTP/blob/master/HARDWARE/SCHEMATICS/Hardware-revision-B/MOD-LCD2.8RTP_RevB.pdf) confirms those signal roles. Pins 3/4 are not needed by this implementation. Touch and backlight control are not implemented; PB8/PB9 are not reconfigured by the display adapter. The ESP-01 remains on Serial6/USART6 (PG9/PG14).

Software SPI is intentional for the supplied GPIO mapping. The display uses the module's reset circuit plus the driver's software reset; there is no separate MCU reset wire. A write-only display cannot be probed for presence, but no read/response wait is performed, so an absent display does not hold up boot beyond the driver's fixed initialization time.

## Build and checks

Keep the working WiFi/motor configuration in ignored `config.h`; do not publish credentials. Do not also select `LCD_DRIVER` or `OLED_DRIVER`.

```sh
pio run -c platformio-olimex.ini -e Nucleo-F439ZI-Olimex
python3 tests/olimex_display_test.py
```

The normal `Nucleo-F439ZI` environment leaves the optional adapter disabled. The display environment pins STM32 platform 19.7.1 and Adafruit ILI9341 1.6.4. PlatformIO also resolves the driver's declared dependencies; their resolved versions appear in its build log.

No full-screen framebuffer is allocated. Two fixed 8x26 character grids retain requested/rendered content. Runtime updates emit at most one changed character per `DisplayInterface::loop()` call. Boot refreshes finish synchronously before `DCC::begin()`. Runtime SPI timing on the actual board still needs measurement; a one-character bound is not a measured execution-time guarantee.

## Hardware acceptance

The build has not been flashed or visually verified yet. Preserve the installed working firmware and a recovery route before testing.

1. With the station powered safely for a firmware test, upload the display-enabled build through ST-Link using the existing upload process.
2. Boot with no USB host attached. Confirm a readable landscape display, firmware/board identity, WiFi setup state, IP address, port, and Ready.
3. Connect an existing DCC/WiThrottle client using the displayed IP and port; check ordinary station operation.
4. Test a boot with the access point unavailable. The display should report unavailable/failure after the existing WiFi setup returns. Existing WiFi setup timeouts are not changed in this patch.
5. Check display orientation, stale character clearing, and station responsiveness during display updates. Confirm that the display's I2C touch hardware does not cause conflicts on the existing bus.

This first patch exposes startup information. Live WiFi reconnection/address-change reporting and Telnet diagnostics remain subsequent milestones.
