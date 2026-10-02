# Producing-inverter comparison: October 1, 2026

This follows the September 30 PlatformIO comparison handoff and the
[night-time verification](nighttime-verification-2026-09-30.md). Times are
Australia/Melbourne, UTC+10. The user reported overcast weather and rain.

The repaired PlatformIO build is left running. It delivered **ten fresh MQTT
inverter scans across three boots**, including the first scan and next two
renewals on every boot. Production was 20–40 W in those validation runs and
60–87 W in the earlier pinned PlatformIO run. No Bluetooth connection failure,
login failure, incomplete scan, panic or unexpected reset was observed in the
repaired-build captures. The pinned ESPHome build delivered no fresh scans in
its 20 minutes of observation, including explicit unpair recovery.

## Pinned comparison result

The full PlatformIO application delivered three fresh producing-inverter MQTT
scans in its first five-minute boot. Its next two complete windows stopped in
Wi-Fi provisioning before Bluetooth initialization. The pinned ESPHome image
delivered no fresh inverter scans across three five-minute windows, or during a
further five-minute extension. ESPHome's encrypted API and Wi-Fi diagnostics
remained available.

PlatformIO therefore supplied useful production data in this session, while the
tested ESPHome image did not. These results do not establish a general Bluetooth
reliability ranking: two PlatformIO windows did not exercise Bluetooth at all,
pairing recovery changed the saved bond, and the windows were sequential.

The subsequent PlatformIO Wi-Fi startup repair is a **different image**. Its
live verification is recorded separately below.

## Image identity and common conditions

| Image | Source | Application bytes | SHA-256 |
| --- | --- | ---: | --- |
| Full PlatformIO with unpair recovery | `1d44bda9f8d268dacdadfd2ddb9f6f6d2a742434` | 1,654,512 | `3ef0aaa6e25a5db90187cf7ab3fa5ef34585b298159df9d14cb9d5ba1c59bd55` |
| Pinned ESPHome | `codex/sma-daytime-verified`, `3c8eb513` | 1,176,336 | `d703492bf696fc015855216b736c34bd4b148b697ccdb2b5222ced0cf70db52a` |
| PlatformIO with Wi-Fi startup repair | Above PlatformIO source plus `src/ESP32_SMA_MQTT.cpp` changes in this report's commit | 1,655,456 | `abd86f94da85f5f5aea739e83f978a71c0e6a173ac1e1dd405fc69bd8943b0e5` |

PlatformIO uses `espressif32@6.3.2`, Arduino 2.0.9 / IDF 4.4.4, board `esp32dev`
and environment `espwroom32`. ESPHome uses 2026.9.0 / IDF 5.5.5; its API confirmed
compilation time `2026-09-30 08:16:35 +1000` on each boot. The previously preserved
ESPHome source passed 34 host tests; it was not changed for this comparison.

USB identity was confirmed as ESP32-D0WD-V3 revision 3.1, MAC
`5C:01:3B:6D:78:80`, 4 MB flash, port `/dev/cu.usbserial-210`. The target was
`00:80:25:2D:23:A9`, serial `2120240167`. No physical relocation, cable or antenna
change was made. Both images contained matching Wi-Fi credentials and inverter
password; equality was checked privately, without printing credentials.

Both used their normal Wi-Fi configuration and a 60-second day scan. PlatformIO
used the existing saved MQTT configuration and automatic SPP discovery, closing
each successful polling session. ESPHome used direct SPP channel 1 and its normal
continuous daytime session design. The native API supplied ESPHome system time;
PlatformIO obtained NTP time on its successful boot. No inverter-clock write was
requested, and the ESPHome clock-write switch remained off.

The same NVS-preserving temporary partition table from the night report was
retained: app0 `0x10000/0x1C0000`, app1 `0x1D0000/0x1A0000`, legacy filesystem
`0x370000/0x20000`, NVS `0x390000/0x70000`. PlatformIO's installed app0 bytes
matched the pinned binary before testing. ESPHome was written to app1 and its
flash hash verified. Subsequent switches changed only the 8 KB OTA metadata,
using valid, increasing sequence numbers and the slot/CRC scheme implemented by
[Espressif's OTA tool](https://raw.githubusercontent.com/espressif/esp-idf/v5.5.5/components/app_update/otatool.py).
The final recovery image was written to app0 and hash verified. The bootloader,
partition table and saved filesystem were retained. No new full-flash backup,
NVS erase or NVS restore was performed.
The checked-in `min_spiffs.csv` layout was not flashed; a standard full
PlatformIO upload uses a different map and needs review before reuse here.

## Complete five-minute windows

Each capture opened USB with DTR/RTS disabled and then performed an explicit EN
reset. The boot banner reported `POWERON_RESET`; times below are seconds after
the host's serial-open marker. Preparatory flashing resets are excluded. Each
listed window lasted 300 seconds.

| Run / local start | Producing-inverter evidence | Connection attempts / failures | First SPP and serial | Login | First consumer scan; next two | Unexpected resets / panics | Consumer availability |
| --- | --- | --- | --- | --- | --- | --- | --- |
| PlatformIO L1, 13:12:46 | 60, 72, 87 W | 5 starts; 2 discovery failures, status 1 | +19.435 s; serial +20.222 s | Success +20.739 s | MQTT +26.388, +84.821, +145.277 s | 0 / 0 | 3 fresh inverter payloads; 5 fresh ESP diagnostics |
| ESPHome E1, 13:22:54 | No inverter measurements | 29 starts; 27 page timeouts; 1 other pre-open failure | +212.634 s; serial +213.922 s | `rc=-5`, no login reply | None | 0 / 0 | Encrypted API connected; no valid sample age |
| PlatformIO L2, 13:28:18 | Not measured | 0 BT starts; waiting for Wi-Fi provisioning | Not reached | Not reached | None | 0 / 0 | No fresh MQTT diagnostics or inverter payloads |
| ESPHome E2, 13:33:21 | No inverter measurements | 30 starts; 28 page timeouts; 1 other pre-open failure; last start unfinished | None | Not reached | None | 0 / 0 | Encrypted API and uptime available |
| PlatformIO L3R, 13:41:42 | Not measured | 0 BT starts; waiting for Wi-Fi provisioning | Not reached | Not reached | None | 0 / 0 | No fresh MQTT diagnostics or inverter payloads |
| ESPHome E3, 13:47:12, first 300 s | No inverter measurements | 30 starts; 29 page timeouts; last start unfinished | None | Not reached | None | 0 / 0 | Encrypted API and uptime available |

L3R was an additional EN restart of the same PlatformIO image, without flashing
or unpairing. A preceding L3 boot at 13:38:23 also entered provisioning, but its
USB capture stopped at +136.569 seconds when the test scheduler was stopped.
That shortened capture is excluded from the complete-window totals. The actual
order was L1, E1, L2, E2, shortened L3, L3R, E3.

The complete comparison windows contain **3 PlatformIO scans in 900 seconds**
(12 scans/hour when normalized over those windows) and **0 ESPHome scans in
900 seconds**. These are observed throughput counts, not estimated long-term
rates or reliability percentages. The extra ESPHome 300 seconds also delivered
zero scans. Connection-attempt counts are different workloads because of the
two firmware session designs.

## Bluetooth stages and unpair recovery

E1 had nine completed page timeouts before the authorized unpair request at
+93.925 seconds. Another attempt was already in flight. Its later connection
completed Bluetooth authentication, opened SPP and returned the expected SMA
serial, but SMA login then returned `E_NODATA` (`rc=-5`). This is a login-reply
timeout, not an observed bad Bluetooth PIN or inverter-password rejection.
The subsequent connection failed before SPP open with raw HCI disconnect reason
`0x08`. E2 also logged one such reason during a failed pre-open attempt.

E1's opened SPP session was deliberately closed after login failed. Its other
SPP close callbacks, and those in E2/E3, mostly describe failed opens rather
than lost working measurement sessions. PlatformIO deliberately disconnects
after each successful read. Those normal closures are distinct from its two
failed discovery attempts in L1.

E3 ran continuously for 600 seconds. A second authorized unpair request was
accepted at +334.336 seconds, after its matched window. Across all ten minutes
there were 60 connection starts, 59 page timeouts, no SPP open and no scan; the
last attempt was unfinished at capture end. Its API uptime advanced from about
2 to 542 seconds. Unpair did not provide dependable recovery in these ESPHome
runs. The ESPHome log confirms that removal was requested, rather than proving
the final bond list; the PlatformIO USB command's absent-bond confirmation was
already verified in the night report.

No Bluetooth authentication-failure callback was observed today, so the
PlatformIO automatic one-per-boot unpair branch remains unexercised by a live
auth failure. It is committed alongside the manually verified USB command.
PlatformIO discovery status 1 does not reveal its lower-level HCI reason and
must not be relabeled as ESPHome's page timeout `0x04`.

## Production, freshness and blocking

L1's independent subscriber received three non-retained inverter payloads with
the expected serial. They reported 236.85–237.42 V, approximately 50 Hz,
33.11–33.13 °C, 0.68 kWh today, 45,097.90–45,097.91 kWh lifetime, status OK and
relay Closed. DC string 1 supplied 78–105 W at 337.85–346.24 V. Bluetooth signal
was 63.92–64.71%. This confirms useful production despite the weather. Production
was not independently measured during the ESPHome windows.

L1's next two serial connection/login/scan sequences matched the later MQTT
messages. Its five diagnostic uptimes were 25/85/145/205/265 seconds; free heap
was 37,876–48,532 bytes. The longest diagnostic-message interval was 61.332
seconds. Diagnostics continued during the last two failed Bluetooth attempts.
This does not measure the longest individual main-loop blockage.

The first valid AC power record appeared on USB at +20.992 seconds. The full
serialized MQTT state was logged at +26.350 seconds and independently received
at +26.388 seconds. The later full serialized states appeared at +84.881 and
+145.498 seconds; small offsets between USB and consumer timestamps reflect
serial buffering and the independent host readers.

L2 entered SmartConfig waiting at +16.862 seconds and stayed there until the
300-second capture ended. L3R did the same. Source inspection shows that a short
initial Wi-Fi attempt can enter `mySmartConfig()`, which waits for a phone packet
before inverter initialization. Its eventual approximately eight-minute reboot
was not observed in these five-minute windows. A missing MQTT payload from these
boots is therefore not a demonstrated Bluetooth or SMA failure.

ESPHome reported startup component operations of 404/405/394 ms on E1/E2/E3.
Its API uptime and Wi-Fi states continued throughout connection retries; inverter
values and sample age remained unavailable. No watchdog panic or unexpected
reset was captured. A continuous scheduler-latency measurement was not added.

Daytime MQTT discovery advertised a 300-second inverter expiry and 180-second
ESP diagnostic expiry. Home Assistant's actual UI expiry was not directly
observed. An initial passive 140-second check saw no fresh legacy messages before
the first USB reset; overnight continuity is consequently unverified.

## Separate PlatformIO startup repair

For builds with `WIFI_SSID` configured, automatic reconnect now starts before
the first connection attempt. Startup waits at most 60 seconds, using a
rollover-safe elapsed-time check, and then continues to the inverter/main loop
if Wi-Fi is still unavailable. It does not automatically enter phone
provisioning for this configured build. NTP startup is attempted only when Wi-Fi
is connected. Builds without compiled Wi-Fi credentials retain provisioning.
Disconnect reason codes are logged without credentials.

The build and `git diff --check` passed. Three live restart captures of this
separate recovery image are recorded in the final validation table below.

| Run / EN reset time | Window | First BT connect / successful SMA login | Fresh MQTT scan times after reset | Fresh scans / ESP diagnostics | Discovery failures / unexpected resets / panics |
| --- | ---: | --- | --- | --- | --- |
| Lfix1, 14:00:17 | 240 s | +18.353 / +19.125 s | +23.131, +79.146, +139.294, +199.158 s | 4 / 4 | 0 / 0 / 0 |
| Lfix2, 14:04:17 | 180 s | +16.102 / +16.605 s | +20.464, +77.847, +139.089 s | 3 / 3 | 0 / 0 / 0 |
| Lfix3, 14:07:18 | 180 s | +10.940 / +11.457 s | +15.386, +74.673, +135.525 s | 3 / 3 | 0 / 0 / 0 |

All ten inverter payloads were non-retained and identified serial `2120240167`.
They reported 20–40 W, 49.95–49.99 Hz, 33.00–33.04 °C, 0.72 kWh today and
45,097.94–45,097.95 kWh lifetime, with status OK and relay Closed. Bluetooth
signal was 65.49–67.45%; diagnostic free heap was 39,844–47,316 bytes. The ten
scans in 600 seconds normalize to 60 scans/hour over these validation windows,
without establishing a long-term success rate.

The first repaired boot acquired an IP at +11.666 seconds after an earlier
Wi-Fi disconnect event. Later restarts also recovered without provisioning.
No further unpair or forced-poll command was needed in these three captures.
The status page returned HTTP 200 at 14:09:22 and contained the expected serial.
The new 60-second offline fallback was not deliberately forced on hardware;
these runs validate recovery with the configured access point available.

These shorter recovery windows and this different binary are not substituted
for the original three five-minute PlatformIO windows. They verify the repair
and show that the full application can repeatedly deliver producing-inverter
data after the ESPHome switch and two further resets. The device remains on
this verified app0 image and its saved configuration.
After the last USB capture closed, MQTT continued to deliver fresh data:
29 W at 14:11:35, followed by a diagnostic uptime of 273 seconds at 14:11:52.
These later messages are not added to the ten-scan validation count.

## Evidence and limits

Private raw USB/API/MQTT logs, image hashes, build/upload logs, source patch and
metadata are under `/private/tmp/sma-daytime-comparison-20261001/`. Credentials
and binaries are excluded from Git. The unrelated `.vscode/extensions.json`
change was preserved.

The first ESPHome installation had a longer flashing/idle gap than later
metadata-only switches. Bond removal, the extra PlatformIO restart and changing
radio/inverter conditions prevent attributing the observed difference solely to
the Bluetooth stack. These are short observations from one ESP32 and inverter.

The night report's decoder and protocol concerns remain: unavailable-value
semantics, reordered/missing DC records, fragmented/malformed traffic, timer
rollover and interrupted-query recovery were not exercised by injecting faults
into this producing inverter. Positive daytime readings do not validate the
zero/sentinel handling seen at night.
