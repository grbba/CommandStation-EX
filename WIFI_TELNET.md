# WiFi console on port 2560

Optional F439ZI firmware adds a Telnet-compatible console on the same ESP-01 WiFi listener as ordinary DCC/WiThrottle clients. It retains the verified Olimex startup display. One console may be active; other sockets remain available to normal clients within the installed ESP AT firmware's connection limit.

## Build, upload, and connect

Use the existing private config.h and the isolated implementation worktree. Build or upload with:

```sh
pio run -c platformio-olimex.ini -e Nucleo-F439ZI-Telnet
pio run -c platformio-olimex.ini -e Nucleo-F439ZI-Telnet -t upload
```

In an iPad Telnet app, set host to the IP shown on the LCD and port to **2560**. Enable **local echo** and use Enter/newline to submit a line. The server refuses remote echo and supports suppress-go-ahead; this avoids duplicate characters when a client provides local echo. Desktop Telnet works the same way. The port remains a raw multi-protocol listener: there is no automatic banner for ordinary throttle clients.

After connecting, type this as the first application line:

```text
#console
```

You should receive a short banner and `dcc> ` prompt. A client that negotiates Telnet options before sending this line is also supported. Normal DCC and WiThrottle clients continue to use their existing first messages, without entering the console.

Example console session:

```text
#console
<s>
diag on
<D RAM>
diag off
help
```

`<s>` requests the station status. `diag on` subscribes this console to new `DIAG()` messages; `<D RAM>` produces a memory diagnostic. Use the usual DCC diagnostic commands (for example `<D CMD ON>` / `<D CMD OFF>`) to control the firmware's diagnostic categories. `diag off` stops remote delivery but does not change those global category settings. The existing serial diagnostic output remains available.

Raw `<commands>` execute through CommandDistributor/DCCEXParser, preserving normal command replies, broadcasts, and asynchronous programming responses. Backspace/Delete edit a line; CRLF and CR-NUL are normalized. Maximum input is 127 ASCII characters per line; longer lines are discarded as a whole. Close the connection from your client to release the console.

## Queues, routing, and limits

- Console input is assembled independently per client, including Telnet negotiation split across packets. Unsupported options are refused; subnegotiation is ignored without accumulating data.
- Four complete commands and eight output messages are buffered with fixed bounds. Command admission reports a full queue. Diagnostic bursts are dropped before consuming the reserved control-message capacity, with a subsequent loss count.
- Diagnostic output starts on a fresh line and restores the prompt/partially typed local-echo input. It is delivered only to the subscribed console; no console greeting or diagnostics are sent to ordinary throttle sessions.
- WiFi transport tracing remains serial-only to prevent diagnostics about transmitting diagnostics from creating an output loop. Old boot diagnostics are not replayed; the LCD and `<s>` provide startup/status information.
- Console commands and ordinary inbound traffic alternate when both are pending, while retaining WiThrottle housekeeping and existing AT CIPSEND scheduling.
- Telnet's literal 0xff bytes use a flagged raw-output path, because RingStream normally interprets 0xff as a flash-string marker. Ordinary command/WebSocket output retains its existing path.
- Disconnect/error handling releases console/subscription state and removes queued messages for that socket while preserving another client's in-flight payload. Session generations reject stale console commands/diagnostics and delayed programming replies after socket reuse. Programming operations themselves are not canceled by closing a console.
- This feature targets the confirmed 32-bit F439ZI setup. Plain Telnet is for the trusted local network; no Internet exposure or authentication service is included.

## Validation and hardware acceptance

Host checks use the real console, WiFi AT framing, RingStream, and extracted programming callback stash methods with fake UART/core endpoints, under AddressSanitizer/UBSan. They cover fragmented input/options, CR handling, edits, overlong lines, mixed throttle/console traffic, WebSocket prescan preservation, raw negotiation output, queue saturation, diagnostic redraw, disconnect/slot reuse, in-flight output cleanup, circular queue compaction, and delayed callback rejection. They do not establish real ESP timing or iPad compatibility.

```sh
python3 tests/telnet_console_test.py
```

Test on the real board:

1. Load the Telnet environment and confirm the display/IP and existing WiThrottle connection still work.
2. Open the iPad or desktop Telnet connection to port 2560, enter `#console`, then `<s>`.
3. Enter `diag on`, then `<D RAM>`; confirm a diagnostic appears only in the console.
4. Operate WiThrottle while sending console commands. Verify turnout/sensor/power updates and continued throttle responses.
5. Close/reopen the console while the throttle remains connected. Repeat diagnostic subscription; it must be off after reconnect.
6. Test a boot without a USB host, plus link interruption and reconnect. Programming callback and slow-client behavior need a dedicated hardware test before release.

To return to the display-only firmware:

```sh
pio run -c platformio-olimex.ini -e Nucleo-F439ZI-Olimex -t upload
```

A repeatable read-only network acceptance script is also available:

```sh
python3 tests/hardware/wifi_console_acceptance.py <station-IP>
```

It requests station status and memory diagnostics, opens a second WiThrottle session for metadata only, checks output isolation, and reconnects the console while the throttle session remains open. It does not acquire locomotives or change power/speed/accessories. On 11 October 2026 this script passed against the user's F439ZI/ESP-01: console status, simultaneous WiThrottle handshake, remote diagnostic delivery/isolation, subscription reset, and retained throttle-session responsiveness. Actual iPad interaction, physical throttle controls, and boot without USB remain user acceptance checks.
