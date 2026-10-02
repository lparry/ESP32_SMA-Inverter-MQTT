# Night-time verification against the PlatformIO comparison handoff

Tested September 30, 2026, Australia/Melbourne. This report covers the full
PlatformIO application, not the earlier minimal Bluetooth probe.

## Result

The fixed full application connected, authenticated and published **four fresh
night-time inverter payloads** to an independent MQTT subscriber. Three followed
the new USB unpair command. Energy today was 2.14 kWh, lifetime energy was
45,097.22 kWh and device status was OK. The fixed build is left running.

Earlier baseline legacy boots failed during service discovery and the ESPHome
controls had page timeouts. The successful recovery run followed explicit unpair
operations and another firmware change, so these sequential runs do not establish
which firmware is more reliable or isolate the cause of the improvement.

No authentication failure was observed in the baseline. Its bond was preserved.
The subsequent user-directed recovery test executed the installed ESPHome unpair
action and added the missing recovery commands to PlatformIO. No inverter-clock
write was performed. Baseline and recovery observations are kept separate below.

## Images and configuration

- PlatformIO source: `153d1b67a846067c433469e4a6760a23541fd0c3`.
  Only `.vscode/extensions.json` was modified before testing. Contrary to the
  handoff's earlier snapshot, `src/SMA_Inverter.cpp` had no uncommitted edits.
- Platform: `espressif32@6.3.2`, Arduino 2.0.9 / IDF 4.4.4,
  `framework-arduinoespressif32@3.20009.0`, board `esp32dev`.
  ArduinoJson 6.21.3, PubSubClient 2.8.0, logger revision `ce0303f`.
- Full application binary: 1,651,808 bytes; SHA-256
  `99ccf830b3f2e08f912b0442777856f99842cb50194291140144cd09fce49eeb`.
- ESPHome control: 2026.9.0 / IDF 5.5.5, compilation time
  `2026-09-30 08:16:35 +1000`, confirmed through the encrypted API.
  OTA binary SHA-256
  `d703492bf696fc015855216b736c34bd4b148b697ccdb2b5222ced0cf70db52a`.
- USB confirmed ESP32-D0WD-V3 revision 3.1, MAC `5C:01:3B:6D:78:80`,
  port `/dev/cu.usbserial-210`. Target inverter `00:80:25:2D:23:A9`,
  expected serial `2120240167`.
- Both configurations used matching Wi-Fi credentials and inverter password.
  The existing private legacy configuration was recovered from the filesystem
  in an existing recovery image, without restoring that image's NVS. It has
  MQTT configured, discovery enabled, a 60-second day scan, UTC+10, and the
  expected serial. The private source defaults have MQTT disabled.
- Legacy uses automatic SPP discovery and a 15-minute night interval, with
  `SUNUP=4`, `SUNDOWN=18`; logged local time confirmed night mode. ESPHome uses
  direct SPP channel 1. Read-only refresh was requested for the ESPHome control.

## Flash layout and preservation

The live partition/OTA metadata selected **app0 at `0x10000`**: valid OTA sequence
9, rather than the handoff's historical app1 selection. The normal PlatformIO
`min_spiffs.csv` layout would relocate NVS. A temporary runtime table instead
retained the current NVS at `0x390000`, size `0x70000`, and the existing bootloader,
OTA metadata, PHY region and app0 address.

| Region | Offset | Test size |
| --- | --- | --- |
| OTA metadata | `0x9000` | `0x2000` |
| PHY | `0xB000` | `0x1000` |
| app0 | `0x10000` | `0x1C0000` |
| app1 | `0x1D0000` | `0x1A0000` |
| Legacy LittleFS | `0x370000` | `0x20000` |
| NVS | `0x390000` | `0x70000` |

The app-only image was written at `0x10000`, never at address zero. The temporary
filesystem was populated with the existing saved legacy configuration. Esptool
reported successful hash verification for each written region. No NVS partition
erase, NVS restore, or new full-flash backup was performed. The later recovery
test explicitly removed the configured inverter bond at the user's request.
The temporary table and recovered configuration are differences from a stock
PlatformIO upload and must be recorded in any later comparison.

## Live observations

Times below are local (UTC+10). Production was not measured: all inverter
readings remained unavailable. Each row uses the image hash recorded above.

| Image / run | USB open + EN reset / capture window | Producing? | Page / discovery failures | SPP open | Login | First full scan | Next two fresh inverter scans | Unexpected resets / panics | Publication verified? |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ESPHome control | 20:20:18 / 150 s | Night; unknown | 6 page timeouts; 7 starts, final attempt unfinished at capture end | None | Not reached | None | None | 0 / 0 | Encrypted API connected; no inverter readings |
| Full legacy, boot 1 | 20:26:10 / 180 s | Night; unknown | 1 discovery failure, status 1, at +15.107 s | None | Not reached | None | None | 0 / 0 | 3 fresh MQTT ESP diagnostics; no inverter state |
| Full legacy, boot 2 | 20:29:48 / 150 s | Night; unknown | 1 discovery failure, status 1, at +16.376 s | None | Not reached | None | None | 0 / 0 | 3 fresh MQTT ESP diagnostics; no inverter state |
| Full legacy, boot 3 | 20:35:11 / 150 s | Night; unknown | 1 discovery failure, status 1, at +13.809 s | None | Not reached | None | None | 0 / 0 | 3 fresh MQTT ESP diagnostics; no inverter state |

Legacy connection requests began at +9.945, +11.216 and +8.623 seconds.
Discovery reported failure about 5.2 seconds later. `BluetoothSerial.connect`
returned later, and the application continued to MQTT discovery/diagnostics.
The baseline legacy logs do not expose an underlying HCI reason for discovery
status 1; it must not be relabeled as the ESPHome control's page-timeout code.
The observed pre-open failure is distinct from SMA login failure.

The three diagnostic uptime sequences were 21/81/141, 23/83/143 and 20/80/140
seconds. Across these payloads, free heap ranged from 42,724 to 49,400 bytes.
Additional diagnostics after the first two capture windows continued to arrive;
these are not extra inverter samples or extra boot attempts.

Serial captures distinguish a USB serial open followed by an explicit EN reset;
the observed boot banner reports `POWERON_RESET`. Preparatory flash/read resets
are excluded from the comparison rows. The ESPHome control and legacy night
schedule do not provide equal attempt counts, and these short sequential runs
are not the matched daytime comparison proposed by the handoff.

MQTT verification used an independent subscriber. Discovery publications and
ESP32 diagnostics are not counted as inverter scans. Fresh diagnostics were
non-retained, and uptime increased across successive messages. The broker
returned 24 retained discovery records; the inverter sensor expiry was 2,700
seconds and ESP diagnostics used 180 seconds. Home Assistant's UI expiry was
not directly observed.

The web status page responded to read-only GET requests. Configuration-route
GET returned 401 without credentials and 405 with credentials; no configuration
POST, SmartConfig action or clock-setting action was sent.

## Source checks against the handoff

These source checks concern the baseline. The recovery run subsequently verified
normal SMA login, query completion and MQTT publication, while malformed input
and unavailable-value semantics remain unverified on hardware:

| Handoff concern | Current source finding |
| --- | --- |
| Failure counting | Only failed Bluetooth connects increment `failCount`, and only during day mode; any successful connect clears it even after later-stage failures. Night failures do not trigger this reboot path. |
| Polling and session design | Fixed hours or a closed relay select day polling. Each legacy poll connects, reads once, and disconnects. The observed windows contained only the first night attempt; a second 15-minute poll was not observed. |
| Receive timeout | `BTgetByte` still busy-waits with a 20-second per-byte deadline using `millis() > deadline`; no whole-transaction deadline or rollover-safe comparison is present. Packet retry exhaustion still restarts the ESP. |
| Fragments and bounds | L1 size, assembled buffer and record-size checks exist, but L2 recognition still expects a contiguous marker and escape state is recreated per L1 fragment. No equivalent legacy host fixture suite was found. |
| Partial night reads | `ReadCurrentData` still aborts at its first failed query, beginning with AC power. The app restores previous data and does not publish an incomplete scan. Thus temperature/energy/status after unavailable AC power remain unverified in this application. |
| Unavailable values and DC identity | Sentinel/negative values can become zero. DC inputs are assigned by arrival order rather than the record's channel. |
| MQTT energy gate | All inverter state publication requires lifetime energy >=10,000 Wh and a nondecreasing counter. The persisted high-water mark is advanced without checking the return values from the NVS writes. Recovery payloads passed the publication gate with lifetime energy of 45,097,220 Wh. |
| Clock/time and web configuration | Clock sync is an explicit action and was not invoked. Numeric UTC+10 does not automatically handle seasonal offsets. Configuration writes require MQTT credentials when a username is set; the root status/configuration display is public. |
| Unpair recovery | The baseline PlatformIO source lacked recovery. The follow-up adds USB `unpair` and `poll`, plus one automatic bond removal/retry per boot when `onAuthComplete` reports failure during an outgoing connection. Manual removal confirms that the configured target is absent from the bond list before scheduling a retry. Discovery status 1 alone does not expose its underlying HCI reason. |

The exact preserved ESPHome source snapshot passed its host suite again:
**34 tests passed**. The PlatformIO build passed. Neither result substitutes for
live inverter measurements.

## User-directed unpair recovery

After the baseline, the original ESPHome image/table were briefly restored and
hash-verified. A second 150-second control at 20:40:13 observed 15 page timeouts,
no SPP open and no panic or automatic reset; its encrypted API confirmed the
morning image. The user then explicitly instructed the unpair recovery to proceed.
The authenticated `Unpair Inverter Bluetooth` action was executed, and ESPHome
logged the bond-removal request. Three subsequent direct-channel attempts still
reported page timeout 4.

The new PlatformIO recovery binary is 1,654,512 bytes; SHA-256
`3ef0aaa6e25a5db90187cf7ab3fa5ef34585b298159df9d14cb9d5ba1c59bd55`.
It was built from the baseline commit plus the recovery source changes described
above, using the same temporary NVS-preserving runtime layout and saved legacy
configuration. The normal authentication failure path now retries promptly even
at night, while the explicit USB command remains available for failures whose
underlying Bluetooth status is hidden by service discovery.

The fixed-build USB capture began at **20:53:11**, lasting 150 seconds:

| Session | SPP/connect success | SMA serial | Login | Fresh MQTT inverter payload |
| --- | --- | --- | --- | --- |
| First attempt following flash and ESPHome unpair | +41.442 s | 2120240167 | 0 / success | 20:53:59 |
| After USB `unpair` | +59.461 s | 2120240167 | 0 / success | 20:54:15 |
| First USB `poll` | +88.884 s | 2120240167 | 0 / success | 20:54:43 |
| Second USB `poll` | +119.661 s | 2120240167 | 0 / success | 20:55:15 |

The USB unpair command was delivered in two pieces, ending with CRLF. It was
recognized once at +47.815 seconds. The legacy stack reported one saved bond
before the request and confirmed that the configured inverter bond was absent
afterward. The next connection started at +53.008 seconds, bypassing the normal
15-minute night wait. An oversized line beginning with `unpair` was ignored;
the next valid `poll` was accepted. There were no incomplete-scan rejections,
panics or unexpected resets. The automatic auth-failure branch was not triggered
by an observed authentication error during this run.

All four inverter payloads were non-retained. They reported 0 W AC power, 2.14 kWh
today, 45,097.22 kWh lifetime, OK status, and 63.53–64.71% Bluetooth signal.
Temperature, voltages, currents and frequency were published as zero; grid relay
was `Information not available`. Because the decoder can map no-data values to
zero, those zeros are **not verified physical measurements**. This confirms useful
night-time energy/status data while preserving the handoff's unavailable-value
concern. The subscriber received two fresh ESP diagnostic payloads as well.

NTP did not become valid during this capture (`failed to obtain time`); the
application used night mode. This differs from the earlier baseline boots and
adds a timing/scheduling confounder. No inverter clock write was sent.

The fixed firmware and temporary runtime table both passed flash hash verification.
The final board runs this full PlatformIO build with the saved legacy configuration
and the recovered/new pairing state. The preserved original ESPHome image/table
remain available privately as a restoration path.

## Evidence and remaining verification

Private logs, images, source/configuration hashes, temporary partition table and
manifest are under `/private/tmp/sma-night-verification/`. Raw logs and saved
configuration contain private details and are not included in this repository.

A producing-inverter comparison still needs alternating repeated boots and fresh
measurements at each consumer under matched conditions. Tonight verified four
successful legacy night scans and MQTT publication after recovery; it does not
resolve daytime reliability, unavailable-value decoding or a comparative success
rate.
