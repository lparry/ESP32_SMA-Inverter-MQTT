# Regression checks

Install PlatformIO 6.1.19, then run:

```sh
pio run -e espwroom32
python3 tests/host/run.py
python3 tests/release/test_publish_firmware_release.py
```

The host runner needs Python 3 and a C++17 compiler with AddressSanitizer and
UndefinedBehaviorSanitizer (Clang is used in CI). It compiles all five production
implementation files with the project's pinned ArduinoJson library. Hardware
interfaces are simulated; the firmware algorithms themselves are not copied into
the tests. Private configuration files are excluded from the temporary test build.

The checks inject failed writes, interrupted saves, hostile HTTP requests, slow
HTTP responses during Bluetooth waits, stale readings, timer rollover,
fragmented and malformed Bluetooth packets, unsupported readings, MQTT
reconnects, and NVS failures. They inspect published JSON and ensure an unmatched
clock reply cannot trigger a clock write. Logging arguments are checked by the
compiler. CI additionally builds with no private configuration, the unchanged
example configuration, and a partial configuration.

`testBluetoothAuthRecoveryMatchesTargetPeer` drives GAP events through the
production registration proxy and a separate SDK fake. It checks peer matching,
callback forwarding, registration failure preservation, and one-time target
bond recovery. The firmware linker wraps the GAP registration symbol so the
observer can inspect peer addresses while forwarding every event to
BluetoothSerial's callback.

## Review finding coverage

This table refers to the original 30-finding review. Later reviews and the
Ralph loop have their own numbered findings and validation records.

| Finding | Regression check |
| --- | --- |
| 1 | `testSaveFailures` |
| 2 | `testSettingsToken` |
| 3 | `testNtpInput` |
| 4 | Missing, example, and partial configuration builds in `run.py` and CI |
| 5 | `testConfigRecovery` |
| 6 | `testLateWifiStartsNtp` |
| 7 | `testMissingNtpDoesNotBlock` |
| 8 | `testStaleRelayExpires` |
| 9 | `testSlowPacketDeadline` |
| 10 | `testBluetoothTimerRollover` |
| 11 | `testLoginAndInitCrc` |
| 12 | `testFragmentEscapes` |
| 13 | `testMalformedRecordRanges` |
| 14 | `testStatusRecordBounds` |
| 15 | Full-width identity assertion in `main` |
| 16 | Header size and restored-alignment static assertions |
| 17 | `testSignedAndUnavailableMeasurements` |
| 18 | `testDcChannels` |
| 19 | `testUnsupportedTemperature` |
| 20 | `testEnergyFilteringDoesNotHidePower` |
| 21 | `testReplacingInverterResetsEnergyBaseline` |
| 22 | `testEnergyPersistenceRetries` |
| 23 | `testDiscoveryCapacity` |
| 24 | `testTopicValidationAndJsonEscaping` |
| 25 | `testProvisionedWifiSurvivesStartup` |
| 26 | `testProvisioningTimeouts` |
| 27 | `testDiscoveryAfterReconnectAndBirth` |
| 28 | Compiler-enforced printf checks and formatted logging in every host run |
| 29 | `testClockReplyCorrelation` |
| 30 | `testExcessFormArguments` |

## Hardware limits

The [19-finding follow-up review](../docs/review-fixes-2026-10-01.md) adds cases
for failed close/readback, filesystem mount failure, escaped page capacity,
password whitespace, stale error replies, RX cleanup, long fragment sequences,
Bluetooth write failure, empty status, initialization trailer bounds, clock
expiry during a read, services during Bluetooth waits, queued publication,
serial-save retries, and discovery migration. These cases were added without
running tests or compiling during that follow-up task. ESP32 durability checks
use the real VFS; the host filesystem fake exercises the shared readback path.
The host configuration enables debug level 3 and syslog so their optional code
paths are included in future host builds.

These tests do not validate radio pairing, actual filesystem/NVS power-loss
behavior, ESPTouch phone compatibility, or live inverter clock synchronization.
SBFspot documents that some inverter firmware does not echo the request ID for
clock replies. This firmware rejects those replies, because it cannot safely
associate their write counter with the current request. An unsuccessful manual
clock request reports failure and does not automatically retry or write the clock.
See the [upstream clock implementation](https://github.com/SBFspot/SBFspot/blob/master/SBFspot/SBFspot.cpp).

No test command flashes the ESP32 or sends an inverter clock update.

## P1 fix coverage and completed validation

`testClockTargetsOneInverter` checks both protocol destination layers and refuses
unknown or broadcast identities. `testBoundedHttpRequests`,
`testBoundedHttpFormCompatibility`, and `testBoundedHttpDeadline` exercise the
production request reader before dispatch, including hostile requests, ordinary
form decoding, cleanup, and slow requests across timer rollover.

On 1 October 2026 the complete host suite passed with sanitizers, the workspace
firmware build passed, and all three CI configuration builds passed with pinned
PlatformIO 6.1.19. See the [P1 fix validation record](../docs/p1-fixes-2026-10-01.md)
for exact build coverage and the remaining hardware limits.

## Ralph loop coverage

The [Ralph loop record](../docs/ralph-loop-2026-10-01.md) maps the subsequent
issue commits to their behavior changes and records complete source coverage.
The host suite now also checks saved-network startup without a compiled SSID,
failed first-time provisioning and marker-save rollback, login identity filters,
Bluetooth RX buffering and early connection handshakes, stale queued readings,
daytime deadline changes, transactional query retries, exact energy persistence
across reboot, CRC-safe logoff, and strict numeric/address settings validation.
Further cases cover interrupted provisioning recovery, storage read errors,
Bluetooth initialization retries and caller deadlines, broker deferral during
Bluetooth reception and HTTP response deferral, discovery migration after long
uptime, fractional timezone and full-width serial reloads, and bounded Basic
authentication on the protected HTTP routes. Empty-network provisioning cases
cover retry after wrong-password and no-phone failures without reboot, backoff
across timer rollover, polling/receive deferral, driver/marker read errors,
credentials installed by another path, and saved-network preservation.

The release suite uses temporary Git repositories and simulated GitHub commands
to check initial publication, reruns, new commits, interrupted tags/uploads, and
API errors, ancestry ordering, interrupted version reservations, and divergent
histories. It makes no live release or repository changes.
