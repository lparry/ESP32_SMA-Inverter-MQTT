# Follow-up review fixes — 1 October 2026

These numbers refer to the 19 findings from the complete source review, rather
than the earlier 30-finding review. Verification for this follow-up is static
inspection only. No tests, compilation, flashing, or live inverter operations
were performed.

| Finding | Change |
| --- | --- |
| 1. Buffered settings writes can lose the good configuration | ESP32 saves check `fwrite`, `fflush`, `fsync`, and `fclose`, then compare the reopened temporary file with the serialized settings. A failed write leaves the committed file and backup alone; successful replacements retain the backup. |
| 2. Loaded values can overflow the HTML buffers | Every append and formatted write checks the page capacity. An oversized page returns an error instead of corrupting memory. |
| 3. Mount failure automatically erases settings | Mount failures use compiled defaults and preserve flash. Filesystem initialization requires the explicit USB `format-config` command. |
| 4. Signed unavailable sentinel is accepted with datatype 0x00 | Reject `INT32_MIN` for both signed and ambiguous numeric records. Known signed records still accept legitimate negative values, including -1. |
| 5. Failed packets leave stale receive bytes | Malformed or incomplete framing closes the connection and drains RX. Disconnect and the next connection also drain RX. |
| 6. Unrelated replies are trusted | Data replies must match the packet ID and inverter identity before status is consumed. Initialization replies must match the request ID and command before installing a serial. |
| 7. Initialization serial overlaps the trailer | The initialization reply must contain at least 64 decoded bytes. |
| 8. Slow scans starve MQTT and web handling | Bluetooth receive waits periodically service MQTT, diagnostics and the web server. The status page displays the latest complete scan. Settings, provisioning and clock submissions return a retry response while polling, preventing reentrant changes. |
| 9. Clock authorization expires during connection/read | Carry the original deadline through clock synchronization and check it again immediately before queuing the clock write. An expired request reports that the clock was unchanged. |
| 10. Bluetooth short writes are ignored | Queue the complete frame in one checked write. Failure closes the connection and propagates to each active caller. |
| 11. Failed MQTT publication discards readings | Keep a snapshot of the latest complete reading and retry failed publication every five seconds when Wi-Fi is available. A newer complete scan supersedes the pending reading. |
| 12. Retained discovery survives identity changes | Persist the previous discovery identity in NVS, remove its retained sensor/diagnostic records before registering another identity, and remove them when discovery is disabled. Failed cleanup preserves the previous identity for retry. |
| 13. Password whitespace is removed | Exclude both password fields from trimming. |
| 14. Failed serial persistence is never retried | Keep the save pending independently of the in-memory serial and retry every 30 seconds. |
| 15. Valid fragments consume the retry allowance | Only packets from an unexpected sender consume that allowance. Valid continuation frames remain bounded by the receive deadline and packet capacity. |
| 16. Debug configuration is ignored or fails at level 3 | Load defaults/overrides before defining debug macros and correct the receive-buffer names in debug dumps. |
| 17. Optional syslog configuration references no appender | Construct the configured UDP appender with static lifetime. |
| 18. Failed Bluetooth signal query publishes stale strength | Initialize and reset the strength to unavailable; failed reads publish JSON null. |
| 19. Empty status vector appears available | Return the unavailable marker when no active attribute exists and serialize unavailable status/relay fields as JSON null. |

Discovery changes also pause when settings storage cannot be mounted, so falling
back to compiled defaults cannot silently delete retained Home Assistant entities.

Regression cases were added or adjusted for these failure paths, but remain
unrun in this task. Host storage fakes exercise readback and replacement logic;
the ESP32-specific durability calls require device-side validation. The Arduino
Bluetooth library's connection call remains synchronous, so opening a connection
can still briefly delay web handling; long receive waits now service it.
Broker reconnection and discovery migration are deferred while a Bluetooth reply
is outstanding, avoiding long network operations inside its receive window.

Discovery migration captures the loaded identity when upgrading older firmware.
Records orphaned by a topic change before this tracking existed cannot be
identified from the current settings alone. Migration targets the configured
broker; changing brokers cannot clean records left on the previous broker.

Implementation references: [Arduino 2.0.9 filesystem wrapper](https://github.com/espressif/arduino-esp32/blob/2.0.9/libraries/FS/src/vfs_api.cpp),
[Arduino 2.0.9 Bluetooth queue and write implementation](https://github.com/espressif/arduino-esp32/blob/2.0.9/libraries/BluetoothSerial/src/BluetoothSerial.cpp),
and [SBFspot packet layouts](https://github.com/SBFspot/SBFspot/blob/master/SBFspot/SBFspot.cpp).
