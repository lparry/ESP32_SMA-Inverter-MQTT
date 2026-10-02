# Ralph loop — 1 October 2026

The requested loop uses parallel GPT-6 Luna agents with max reasoning. Each
confirmed issue is fixed in an isolated worktree and committed separately. After
integration, the complete host suite and firmware build configurations are
checked, followed by another review of every source file. The stopping condition
is a complete review with no further actionable findings, or 20 rounds.

The loop stopped on 2 October after eight fix rounds and 32 separate issue
commits. The ninth complete review found no further actionable defects across
all 40 source files, the private configuration, and build/workflow configuration.
All host checks, ten release regressions, and four firmware configurations were
green at the last production commit, `adf65fd`.

The finding counts were **8 → 5 → 8 → 5 → 2 → 2 → 1 → 1 → 0**.

## Round 1

The eight findings from the preceding static review were fixed separately:

| Issue | Change | Integrated commit |
| --- | --- | --- |
| 1 | Saved Wi-Fi credentials receive a bounded connection attempt instead of automatically entering SmartConfig when the AP is unavailable. | `ce3fee6` |
| 2 | Login replies must match the configured Bluetooth sender, known identity, request ID, command and timestamp. | `a890583` |
| 3 | The SPP callback buffers incoming Bluetooth data while network services run; detected overflow closes and resets the session. | `196cb15` |
| 4 | Queued measurements expire using their acquisition time and polling-mode expiry window. | `e36be8a` |
| 5 | A transition to faster daytime polling shortens an outstanding nighttime deadline while preserving earlier explicit requests. | `01214a7` |
| 6 | Failed multi-packet measurement queries restore inverter and display data before a retry. | `b811b52` |
| 7 | The exact lifetime-energy counter is persisted before it can be published, including increments below 1 kWh. | `3960850` |
| 8 | Optional logoff requests retry packet IDs whose unescaped CRC contains a reserved framing byte. | `e9ebed8` |

The combined host suite passed with AddressSanitizer and
UndefinedBehaviorSanitizer. Firmware compiled and linked with the workspace
configuration and all three CI public configurations: defaults, the unchanged
example, and a partial configuration defining only `SCAN_RATE`. Builds used
PlatformIO 6.1.19, ESP32 platform 6.3.2, Arduino core 2.0.9, and the pinned project
dependencies. The resulting RAM usage was 84,532 / 327,680 bytes, and workspace
flash usage was 1,665,289 / 1,966,080 bytes.

The subsequent complete review after all eight fixes found five additional issues:
early Bluetooth handshake loss during connection, stale model identity after
an inverter serial changes, failed first-time provisioning recovery, incomplete
numeric form parsing, and incomplete Bluetooth-address validation.

## Round 2

| Issue | Change | Integrated commit |
| --- | --- | --- |
| R2.1 | New-session Bluetooth RX is accepted during the blocking connection call; prior and failed sessions are cleared. | `e03c63d` |
| R2.2 | Failed first-time provisioning restores an empty station configuration after disconnecting; existing credentials are restored and unreadable credentials are preserved. | `636c1a5` |
| R2.3 | Numeric settings require complete, finite, range-safe parsing before any submitted changes are saved. | `23b436e` |
| R2.4 | A validated serial change clears the prior model identity so the correlated login can learn the replacement model. | `e90e3b4` |
| R2.5 | The settings form and startup share exact Bluetooth-address parsing; malformed saved addresses disable polling instead of partially decoding. | `93e7f11` |

At `93e7f11`, the complete host suite passed with both sanitizers, and all four
firmware configurations compiled and linked. RAM usage remained 84,532 bytes;
flash usage was 1,665,985 bytes for the workspace and 1,665,785 bytes for each
public configuration. The next complete review rotated file assignments between
reviewers and found eight additional issues.

## Round 3

The findings are discovery-identity reads that conflate storage errors with an
absent record, failed Bluetooth initialization without recovery, per-byte address
wildcards, ignored SmartConfig startup failure, provisioning interrupted by a
reset, blocking MQTT reads during Bluetooth receive, packet timeouts that extend
caller deadlines, and provisioning-flag reads that can overwrite saved Wi-Fi
credentials on a storage error.

| Issue | Change | Integrated commit |
| --- | --- | --- |
| R3.1 | Discovery identity remains untouched on storage/read/type errors and retries before cleaning old retained topics. | `9cd2c13` |
| R3.2 | Bluetooth initialization retries with capped backoff; polling and unpairing wait while network and serial services continue. | `bf48cf5` |
| R3.3 | A sender is a wildcard only for the complete broadcast address; configured `FF` octets are matched exactly. | `541dcd5` |
| R3.4 | A failed SmartConfig start rolls back immediately without entering the phone-provisioning wait. | `a555f58` |
| R3.5 | A versioned recovery journal restores the prior network and setup flag after interrupted provisioning; uncertain recovery defers station startup and retries. | `33b5472` |
| R3.6 | Broker reads, reconnects, discovery and status publication wait until Bluetooth reception finishes. | `b77633e` |
| R3.7 | Byte reads honor the earlier caller deadline across handshake stages, mismatched replies, partial frames and timer rollover. | `f82f2a7` |
| R3.8 | Missing setup state selects compiled Wi-Fi defaults; ambiguous reads preserve the driver-owned network. | `07257ae` |

At `33b5472`, on 2 October, the complete host suite passed with both sanitizers,
and all four firmware configurations compiled and linked. RAM usage was
84,556 / 327,680 bytes; flash usage was 1,667,793 / 1,966,080 bytes for the
workspace and 1,668,365 bytes for each public configuration. A firmware compiler
failure in the new static helpers was repaired by using the pinned logger's
global logging calls before repeating the complete checks. The repair remains
part of the provisioning-journal commit.

The subsequent complete review found five more actionable issues: fractional
timezone and full-width serial values lost by JSON fallback typing, unsafe
Basic-auth allocation in the pinned WebServer implementation, a discovery retry
deadline left stale through long uptime, and inaccurate provisioning failure text.

## Round 4

| Issue | Change | Integrated commit |
| --- | --- | --- |
| R4.1 | Timezone reload accepts fractional and integer offsets, checks finite bounds, and uses a safe compiled fallback. | `af0cf09` |
| R4.2 | Serial reload validates the complete unsigned 32-bit range before the startup save or discovery migration. | `f375c83` |
| R4.3 | Setup messages distinguish successful restart from failed setup that restores the prior network and resumes operation. | `be12f20` |
| R4.4 | Discovery migration retries use unsigned elapsed time with explicit attempt state, including after long idle periods. | `567d7ae` |
| R4.5 | Protected routes compare bounded Basic credentials without the pinned WebServer allocation overflow and preserve the normal authentication challenge. | `9652b86` |

At `9652b86`, on 2 October, the complete host suite passed with both sanitizers,
and all four firmware configurations compiled and linked. RAM usage was
84,556 / 327,680 bytes; flash usage was 1,664,197 / 1,966,080 bytes for the
workspace and 1,664,861 bytes for each public configuration. The existing
blank-MQTT-username admin access remains the bootstrap policy; configured
credentials are verified safely. The subsequent complete review found two more
actionable issues: synchronous HTTP response writes during Bluetooth waits and
duplicate release versions on workflow reruns.

## Round 5

| Issue | Change | Integrated commit |
| --- | --- | --- |
| R5.1 | HTTP handling waits until Bluetooth receive ends, including responses to malformed requests; a simulated 25-second response stall no longer consumes the reply deadline. | `f4c98ac` |
| R5.2 | Release reruns reuse the same commit's tag, skip complete releases, and repair interrupted uploads in place; API failures stay fatal. | `67eaf7f` |

The protocol/utilities and application/test reviewers found no further
actionable defects after reading all assigned files. The new release suite
uses temporary Git repositories and simulated GitHub commands without
publishing a release.

At `67eaf7f`, on 2 October, both host binaries passed with AddressSanitizer and
UndefinedBehaviorSanitizer, all six release regressions passed, and all four
firmware configurations compiled and linked. RAM usage was
84,556 / 327,680 bytes; workspace flash usage was 1,664,209 / 1,966,080 bytes;
each public configuration used 1,664,861 bytes. The next complete review
covered all 35 source files and found two release-scheduling issues.

## Round 6

Pending release jobs can be replaced when multiple matrix builds finish,
including replacing a newer main commit with an older slower build. Separately,
source commits can receive version numbers out of ancestry order.

| Issue | Change | Integrated commit |
| --- | --- | --- |
| R6.1 | Release jobs share a serialized queue that retains up to 100 pending runs instead of replacing the waiting job. | `22d4b4c` |
| R6.2 | An ancestor build cannot allocate a newer version after a descendant tag has reserved one, including interrupted releases; same-commit repair and divergent histories remain supported. | `912c17f` |

At `912c17f`, both host binaries passed with sanitizers, all ten release
regressions passed, and all four firmware build checks passed. Firmware source
was unchanged in round six; RAM and flash usage stayed at the round-five values.
The next complete static review covered all 35 source files and found one
remaining bootstrap recovery issue.

[GitHub documents pending-job replacement and the optional queue setting](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency).

## Round 7

With no compiled or saved Wi-Fi credentials, a timed-out or rejected first
SmartConfig attempt restores the empty station configuration but never accepts
a second phone attempt until reboot. The other reviewers found no additional
actionable defects.

| Issue | Change | Integrated commit |
| --- | --- | --- |
| R7.1 | Confirmed-empty setup retries after one minute, backs off to five minutes, defers during Bluetooth receive/polling, and cancels if credentials appear; storage ambiguity defers provisioning. | `d002b3e` |

At `d002b3e`, both host binaries passed with sanitizers, all ten release
regressions passed, and all four firmware configurations compiled and linked.
Workspace RAM and flash usage stayed at 84,556 and 1,664,209 bytes. Public
configurations used 84,564 bytes of RAM and 1,665,657 bytes of flash. Regressions
cover wrong-password and no-phone failures followed by successful setup without
reboot, timer rollover, backoff, receive/poll deferral, driver/marker read errors,
external credential installation, and preservation of a saved network.

The next complete review covered all 35 source files with assignments rotated.
It found one Bluetooth authentication-correlation issue; the other coverage was
clean.

## Round 8

The pinned core forwards authentication completion as a boolean, dropping the
peer address. A nearby device's failed pairing attempt during a failed outgoing
inverter connection can therefore trigger removal of the inverter's saved bond.
The fix preserves the core GAP callback while observing the peer address and
requiring it to match the current target.

| Issue | Change | Integrated commit |
| --- | --- | --- |
| R8.1 | A GAP registration proxy forwards all core events and records auth failure only for the active inverter address; a short lock protects handler publication and attempt state. | `adf65fd` |

At `adf65fd`, both host binaries passed with sanitizers, all ten release
regressions passed, and all four firmware configurations compiled and linked.
Workspace RAM/flash usage was 84,572 / 327,680 and 1,664,345 / 1,966,080 bytes;
each public configuration used 84,596 bytes of RAM and 1,665,813 bytes of flash.
The linked ELF for each configuration confirms that BluetoothSerial's
registration calls the proxy and the proxy calls the original SDK function.

Host regressions cover event delivery from a second thread at registration,
failed and null registration preserving the previous handler, core callback
forwarding, idle/unrelated/success events, a matching target failure, and
one-time recovery. The adapter and its host SDK fake add five source files,
bringing the inventory to 40.

The review and implementation use the exact Arduino 2.0.9 / IDF 4.4.4
dependencies validated by the builds.
[The pinned BluetoothSerial implementation](https://github.com/espressif/arduino-esp32/blob/2.0.9/libraries/BluetoothSerial/src/BluetoothSerial.cpp),
[the pinned SDK registration implementation](https://github.com/espressif/esp-idf/blob/v4.4.4/components/bt/host/bluedroid/api/esp_gap_bt_api.c#L23-L35),
and [GNU's linker wrapping documentation](https://sourceware.org/binutils/docs/ld/Options.html#wrap)
support the callback and adapter analysis.

## Final review: round 9

All three reviewers completed their assigned files in full at `adf65fd`, with
assignments rotated from the preceding pass. The protocol/utilities/observer
review covered six files; the application/host/stub review covered 26; the
MQTT/configuration/release review covered eight source files plus four build and
README files. The private configuration was inspected with credential values
redacted. No new actionable defect was confirmed, so the requested clean-review
stopping condition was met without reaching the 20-round cap.

No tests or compilation were run during the review itself. The combined
validation immediately preceding it remains applicable because the review made
no source changes. Final documentation edits do not alter the tested firmware
or test implementation.

The 32 temporary issue worktrees were removed after checking that they were
clean and contained no private configuration. Their issue branches and commits
are retained. Public build copies and validation logs remain outside the
repository; no live release was created or pushed.

## Source coverage

Every tracked source file is reviewed in each complete pass. The inventory
started with 12 production files under `src`, four files directly under
`tests/host`, and 17 host stubs. Round five added the release publisher and its
Python regression suite, bringing the count from 33 to 35. Round eight added
two production observer files, two host SDK fake files, and one API stub. The
current inventory is 14 production files, six host files, 18 stubs, and two
release files: 40 source files in total. The private configuration's directives
are inspected without displaying credentials. Build configuration, the CI
workflow and the project/test READMEs are also checked. No review stops at a
fixed number of findings.

## Validation commands and limits

```sh
python3 tests/host/run.py
python3 tests/release/test_publish_firmware_release.py
pio run --environment espwroom32
git diff --check
```

Public configuration builds use isolated copies that exclude private overrides.
The host runner exercises production implementations with simulated hardware.
No ESP32 is flashed, and no live inverter clock is changed by these checks.
Actual radio timing, pairing, ESPTouch behavior and storage durability during
physical power loss still require hardware validation.

The receive queue adds 4 KiB of static RAM; transactional query snapshots add
about 2.5 KiB. The energy fence writes NVS whenever an accepted counter increases
before publication. This closes the reboot window at the cost of more flash
writes. Storage failures suppress only lifetime energy, keeping other live
measurements available and retrying storage on later readings.

The unrelated local `.vscode/extensions.json` edit is preserved.
