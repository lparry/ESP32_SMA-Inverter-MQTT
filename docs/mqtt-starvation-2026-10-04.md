# MQTT starvation — 4 October 2026

## Field evidence (firmware `3245264`)

Home Assistant showed the inverter and ESP diagnostic sensors unavailable for
one to three hours at a time, day and night. The ESP had not rebooted: `Uptime`
kept counting across each gap (731 s at 05:07, 12,906 s at 08:27). Free heap was
steady at about 47 KB, so memory was not the cause. Only two reboots were seen in 24 hours.

The Mosquitto log showed the same pattern for every session:

```
19:59:10 New client connected ... as SMA-3FFD2ADC (p4, c1, k120, u'lucas')
20:04:48 Client SMA-3FFD2ADC ... disconnected: exceeded timeout.
```

Each session sent nothing at all, not even a keepalive or the 60-second status,
for 1.5 × the 120 s keepalive. The next attempt came 7–60 minutes later. Twice,
a TCP connection opened without ever sending MQTT CONNECT.

Inverter reading intervals were mostly 60–90 s and up to about 4 minutes, with
the scan rate set to 60 s. Bluetooth signal was 65–69 %.

## Cause

1. While Bluetooth waited for a reply, all network work was suppressed on
   purpose: `wifiLoop(true)` skipped `client.loop()`, publishing and HTTP.
   That came from earlier fixes R3.6 and R5.1. Each wait could last 20 s, each
   query 30 s, and a poll makes about ten waits.
2. The next poll was scheduled from the *start* of the current one. A poll
   longer than the scan interval was followed immediately by another, which left
   MQTT one 100 ms loop pass between polls.
3. Every broker reconnect called `requestDiscovery()`, which set `firstTime`.
   Readings were held back until roughly 30 retained discovery messages had been
   republished. The next poll ran before that publish, and by the time it ended
   the broker had dropped the session. So the loop repeated: reconnect,
   long poll, timeout.

## Changes

| Step | Change |
| --- | --- |
| Diagnostics | `esp/state` adds `LastPollMs`, `MaxPollMs`, `LastPollTimeouts`, `LastPollResult`, `Polls`, `PollFailures`, `MqttConnects`, `ResetReason` and `PollTask`. Seven of these are also Home Assistant diagnostic sensors. |
| Discovery | A reconnect no longer forces rediscovery. Readings are held only until this inverter's entities have been announced once. The HA birth message, identity changes and day/night expiry changes still republish. |
| Timeouts | Reply timeout 20 s → 8 s, query timeout 30 s → 12 s, plus a new 90 s whole-poll budget. All can be overridden in `config_values.h`. |
| Scheduling | The next poll is scheduled from when the previous poll *ends*. A USB `poll`/`unpair` command or a clock request made during a poll runs once that poll finishes. |
| Poll task | Connect → login → read → disconnect runs on a dedicated FreeRTOS task (`sma-poll`, 8 KB stack, priority 1, Arduino core). The main loop services Wi-Fi, MQTT and HTTP throughout. Results come back through an IDLE/RUNNING/DONE hand-off, and the main loop owns all publishing, scheduling and reboot decisions. If the task cannot be created, the old inline path, including the receive-wait service callback, is used. |

## Ownership rules for the poll task

- **IDLE / DONE:** the main loop owns `smaInverter`, `invData` and `dispData`.
- **RUNNING:** the poll task owns them, plus `activeJob` and `pollResult`. The
  main loop may still read the 32-bit `invData.Serial` for discovery topics, as
  before. Serial saving and grid-relay freshness read inverter data only while idle.
- **Inverter commands:** USB `unpair` is now executed by the poll task before its
  next connection, not from the main loop.

## Validation

- **Host suite:** passes under AddressSanitizer and UndefinedBehaviorSanitizer,
  with new tests for re-announcement not holding readings, configurable
  timeouts and poll budget, end-of-poll scheduling and clock-request
  generations, and ESP status health fields.
- **Firmware:** compiled and linked offline with the ESP32 GCC 8.4.0
  `esp-2021r2-patch5` toolchain against arduino-esp32 2.0.9, using the
  PlatformIO `espwroom32` flags, because the PlatformIO registry was not
  reachable. Run `pio run -e espwroom32` locally before flashing.
- **Not covered:** host builds run the poll synchronously, so the concurrency
  between the task and the main loop has only been checked by review. It still
  needs to be confirmed on hardware.

## What to watch after flashing

- **`MqttConnects`** should stay at 1–2 per day. Before, it reconnected every few minutes.
- **`LastPollMs` / `MaxPollMs`** show how long reads really take. If polls are
  often near 8 s per step, raise `SMA_REPLY_TIMEOUT_MS`. If they hit the 90 s
  budget, look at which query stalls.
- **`LastPollResult`** at dawn and dusk shows whether the inverter's Bluetooth is
  half awake (`reply timeout`) or off (`no connection`).
- **`ResetReason`** explains any remaining reboots.
