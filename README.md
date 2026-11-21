# BITAXE_AUTOTUNER
An autotuner program with a web interface which keeps you BitAxe(s) in a given temp range, for e.g. minimum 64°C and maximum 66°C.
When environment's temperature raises the program will lower the miner's clock, while during the night the speed will normally increase.

<img width="1366" height="768" alt="immagine" src="https://github.com/user-attachments/assets/b9a16831-bc59-416e-bd3b-82914642f86d" />
*The main screen*

<img width="923" height="200" alt="immagine" src="https://github.com/user-attachments/assets/94c57b0a-807e-4f88-a5a2-5b999f415af3" />
*The settings are simple and the tooltip contains a brief description of the logic*

<img width="1363" height="338" alt="immagine" src="https://github.com/user-attachments/assets/c21c8cd8-fee3-4bc2-8bf2-1f4c16be5639" />
*The device list with the enable/disable auto-tuning checkbox and the VCore voltage offset*

<img width="1350" height="532" alt="immagine" src="https://github.com/user-attachments/assets/d4711361-0ac0-479e-86f3-059be55cf222" />
*The voltage ladder, the maximum I reached with my cooling system is 1020MHz @1.335V*

<img width="1355" height="360" alt="immagine" src="https://github.com/user-attachments/assets/cb56eadf-dd4c-43c7-8836-3b0b42c6af75" />
*The time/NTP servers to synchronze the ESP's clock*

<img width="1306" height="348" alt="immagine" src="https://github.com/user-attachments/assets/fbecbf1e-b212-4965-bac0-be8b3fc7ad6f" />
*The log*

# BITAXE AUTO-TUNER - COMPLETE FEATURE LIST

**1. WIFI CONNECTIVITY**
----------------------
- Connects automatically to a configured WiFi network (SSID + password
  set as constants in the sketch).
- On boot, waits up to 30 seconds for the connection, printing progress
  dots to the Serial monitor, then prints the assigned IP address once
  connected.
- If WiFi drops during operation, reconnect attempts are made
  automatically, rate-limited to at most once every 5 seconds (instead
  of hammering the WiFi driver every loop iteration, which can
  destabilize it).


**2. NTP TIME SYNC**
-------------------
- Synchronizes real wall-clock time via NTP on boot, using two NTP
  servers (pool.ntp.org and time.google.com as fallback).
- Timezone is configurable via a POSIX TZ string; set to Europe/Rome by
  default, which automatically handles the CET/CEST daylight-saving
  time switch with no manual adjustment needed.
- Used to timestamp every entry in the per-device operation log (e.g.
  "2026-07-31 00:30:02").
- If NTP hasn't synced yet (e.g. right after boot, before WiFi is up),
  falls back to showing device uptime instead ("up+00:04:12") so log
  entries are never left blank.
- Keeps retrying NTP in the background until it succeeds, then switches
  over to real timestamps automatically.


**3. CORE AUTO-TUNING LOGIC**
----------------------------
- Automatic frequency/voltage tuning based on live ASIC temperature
  readings from the BitAxe.
- User-defined "ladder" of {frequency, coreVoltage} steps, ordered from
  safest/coolest to most aggressive/hottest.
- DECREASE: when temp rises above the "Decrease above" limit, steps
  DOWN to a lower (safer) ladder position. Does nothing if already at
  the first (safest) position.
- INCREASE: when temp drops below the "Increase below" limit, steps UP
  to a higher (more aggressive) ladder position. Does nothing if
  already at the last position.
- Cooldown timer (10 seconds) enforced after every change, so the ASIC
  has time to physically react to the new setting before being
  re-evaluated (prevents rapid oscillation between settings).
- Control-loop cadence: checks each device every 5 seconds. This is
  intentionally independent from the dashboard's own auto-refresh
  setting (see section 7).
- Ladder auto-sync on boot: on startup, and again any time a device
  reconnects, the ESP32 reads the BitAxe's ACTUAL currently running
  frequency/voltage and finds the closest matching ladder position,
  instead of assuming a starting state. This means manual changes made
  directly in the AxeOS web UI never confuse the auto-tuner.


**4. MINIMUM / MAXIMUM TEMPERATURE PROTECTION**
----------------------------------------------
- MINIMUM_TEMP (user-editable, default 45C, 1C steps): below this
  temperature (e.g. right after the BitAxe reboots or reconnects and is
  still cold), increase/decrease is paused entirely - checked before
  the cooldown timer, and does not consume/reset the cooldown.
- Post-recovery "settle window" (user-editable, default 60s, 1s steps,
  0-120s range): once temp climbs back above MINIMUM_TEMP, the tuner
  waits this long before resuming normal increase/decrease evaluation,
  rather than acting on the very first borderline reading after
  recovery. This is a one-time timer per recovery event, correctly
  re-armed on every new below-to-above transition.
- MAXIMUM_TEMP: fixed at 70C, NOT exposed in the UI or user-editable.
  Acts as a hard safety ceiling applied to all three user-editable
  temperature values (Decrease above, Increase below, Minimum temp) -
  none of them can be saved above this value. The settle window has its
  own separate 0-120s range, since it is a duration, not a temperature.
- Validation rule: "Decrease above" must be at least 1.0C higher than
  "Increase below". Checked both client-side (instant feedback, no
  round trip) and server-side (authoritative, in case JS is bypassed).
- MAX_VCORE_MV: fixed at 1350 (mV), NOT exposed in the UI or
  user-editable. A second, independent hard safety ceiling (separate
  from MAXIMUM_TEMP) that bounds the ACTUAL voltage ever sent to a
  BitAxe. See section 14 (Voltage Offset) for how this is applied,
  since it constrains the combination of a ladder step's voltage AND
  the global voltage offset together, not either one in isolation.


**5. MULTI-DEVICE SUPPORT**
--------------------------
- Manage up to 16 BitAxe units from a single ESP32.
- Each device tracks its own: name, IP address, ladder position,
  cooldown timer, minimum-temp/settle state, operation log, and NVS
  storage namespace - fully independent of every other device.
- Devices are enabled/disabled via a simple config array at the top of
  the sketch. Name and IP address are seeded from that array on first
  boot, but are runtime-editable thereafter from the dashboard - see
  section 15 (Edit BitAxe Name / IP).


**6. PERSISTENT SETTINGS (STORED IN FLASH / NVS)**
--------------------------------------------------
- The following are saved to flash and survive reboots:
    * Decrease-above temperature limit
    * Increase-below temperature limit
    * Minimum temperature
    * Post-recovery settle time (seconds)
    * Dashboard auto-refresh interval
    * Voltage offset (mV) - see section 14
- All of the above are editable live from the web UI's Settings panel,
  with changes taking effect immediately (no reboot required).


**7. PER-DEVICE OPERATION LOG**
--------------------------------
- Stores the last 10 auto-tune actions per device, in a ring buffer.
- Each entry records: real timestamp (via NTP, see section 2), the
  action taken (INCREASE or DECREASE), and the before/after frequency
  and voltage. Voltage values logged are always the EFFECTIVE voltage
  (ladder step + voltage offset, see section 14), matching what was
  actually sent to the BitAxe and what the dashboard displays.
- A device's IP/name change (section 15) is also written to its
  operation log as its own entry (action "IP CHANGED" / "NAME
  CHANGED"), so the history of a given slot stays traceable across
  reassignment to a different physical unit.
- Persisted to flash (NVS) independently per device, survives reboots.
- "Clear log" button per device, with a confirmation prompt before
  wiping it, backed by a dedicated /clearlog server endpoint.


**8. LIVE WEB DASHBOARD**
--------------------------
- Single-page dashboard served directly by the ESP32 itself.
- Updated via AJAX (JavaScript fetch() polling a JSON endpoint) instead
  of full page reloads - no flicker, and scroll position / open-closed
  panel states are preserved between updates.
- Adjustable polling interval via a dropdown: 5s / 10s / 30s / 60s /
  5 minutes. This only controls how often the BROWSER re-fetches data;
  it does not change how often the ESP32 itself talks to the BitAxe,
  which stays fixed at 5 seconds (explained via tooltip, see below).
- Per-device stats shown, refreshed live:
    * Hashrate (TH/s)
    * Error rate (%) - read directly from the firmware's own
      errorPercentage field, not recalculated from share counts
    * Efficiency (Watts per TH/s)
    * Best difficulty, all-time and current session - formatted with
      K/M/G/T suffixes for readability (e.g. "9.78G", "12.29M")
    * Uptime, formatted as Xd Xh Xm Xs
    * Power draw (W)
    * Input voltage (V)
    * ASIC frequency (MHz)
    * ASIC voltage - explicitly labeled "measured", since this is the
      voltage regulator's ACTUAL output reading, not the setpoint you
      configured (small deviations of a few mV between the two are
      normal regulator behavior, not a bug)
    * ASIC temperature (C), color-coded red/blue when outside the
      configured limits
    * VR (voltage regulator) temperature (C)
- Status banners per device (each independently shown/hidden):
    * Red "Device unreachable" banner when the BitAxe can't be reached,
      while still showing the last known data.
    * Yellow "ASIC temp below minimum - auto-tune paused" banner.
    * Blue "Stabilizing after minimum-temp recovery" banner with a live
      countdown of seconds remaining, during the (configurable) settle
      window.
- Settings panel is a collapsible accordion, closed by default on every
  page load (built with native HTML <details>/<summary>, no JavaScript
  needed to expand or collapse it).
- Tooltips: a small tappable "i" icon next to every settings field,
  explaining exactly what it does and how it interacts with the rest of
  the system. Works via mouse hover on desktop AND via tap-to-focus on
  mobile (pure CSS, no JavaScript, and keyboard-accessible via
  tabindex/aria-label).
- Responsive operation-log table: scrolls horizontally on narrow/mobile
  screens instead of squeezing or breaking the page layout.
- Forces English locale on all numeric input fields, so the decimal
  separator is always a dot (e.g. "65.0"), regardless of the browser's
  or the operating system's regional settings (fixes an issue where
  Firefox on a non-English Windows install displayed commas instead).
- Voltage values are formatted using exact integer math (not floating
  point), so displayed values like "1.245 V" are always precise and
  never affected by binary floating-point rounding artifacts.
- Auto-refresh, temperature limits and footer text all update instantly
  in the page after saving settings, with no full page reload.


**9. RELIABILITY / SELF-HEALING**
----------------------------------
- Heartbeat LED: flashes briefly (150ms) every time the 5-second
  control-loop cycle actually runs, so you can tell at a glance the
  sketch is alive and the main loop is not stuck. A solid-on (non-
  blinking) LED is a clear visual sign of a frozen loop.
- Hardware watchdog timer (20 seconds): if the main loop (or setup)
  ever fails to return control for that long, the ESP32 automatically
  reboots itself - no manual power-cycle needed. Fed at the top of
  every loop iteration, during the WiFi-connect wait at boot, and again
  after each device is processed.
- Explicit TCP connect timeout (in addition to the response-read
  timeout) set on every single HTTP call made to a BitAxe, since the
  connect phase alone can otherwise hang far longer than expected after
  a network outage.
- Because settings and per-device logs are already persisted in flash
  and the ladder position always re-syncs from the live BitAxe on boot,
  an automatic watchdog reboot recovers cleanly with zero data loss.


**10. BITAXE / AXEOS API INTEGRATION**
---------------------------------------
- GET  /api/system/info    - reads all live stats used by the tuner
                              and the dashboard.
- PATCH /api/system        - applies a new frequency + coreVoltage.
- POST /api/system/restart - implemented and ready to call, but left
                              DISABLED by default, since this firmware
                              applies frequency/voltage changes on the
                              fly (matches real-world testing - no
                              reboot needed after a change via the
                              AxeOS UI either). Can be re-enabled with a
                              single flag if a future firmware version
                              needs it.


**11. ESP32 WEB SERVER ENDPOINTS**
------------------------------------
- GET  /              - dashboard page (initial full render).
- GET  /api/status    - JSON status for all enabled devices, polled
                         repeatedly by the dashboard's JavaScript.
- POST /savesettings  - saves temperature limits + refresh interval,
                         returns the saved values as JSON.
- POST /clearlog      - clears one specific device's operation log.
- POST /editdevice    - edits one device's name and/or IP address,
                         returns the saved values as JSON (see section
                         15).


**12. HARDWARE NOTES (HEARTBEAT LED)**
----------------------------------------
- Automatically uses the board's LED_BUILTIN pin if the board's Arduino
  core defines one.
- Falls back to a configurable GPIO (GPIO4 by default) if the board's
  core does not define LED_BUILTIN, or if the board (like a generic
  ESP32-WROOM-32 DevKit clone) has no user-controllable onboard LED at
  all - in that case, a simple external LED plus a ~220-330 ohm
  resistor wired to that GPIO and GND works as a drop-in replacement,
  no code changes needed.


**13. VOLTAGE OFFSET**
----------------------
- A single global signed value, user-editable from the Settings panel,
  range -50mV to +50mV, in 1mV steps.
- Persisted to flash (NVS), survives reboots, part of the same
  Settings save/load path as the temperature limits (see section 6).
- Applied at send-time only - the ladder itself (see section 3) is
  never modified. The EFFECTIVE voltage actually sent to a BitAxe via
  PATCH /api/system is always: ladder[step].coreVoltage + offset.
- The dashboard and the per-device operation log always display this
  EFFECTIVE voltage (ladder value + offset), never the raw ladder
  value, so what's shown always matches what the ASIC is actually
  running.
- Boot / reconnect ladder auto-sync (section 3) is offset-aware: the
  live voltage read back from the BitAxe is matched against
  ladder[i].coreVoltage + offset for each step, not against the raw
  ladder values, so the closest-match logic stays correct regardless
  of the currently configured offset.
- Changing the offset takes effect in real time: as soon as it's
  saved, each affected device's effective voltage is recalculated. If
  a device is currently inside its 10-second post-change cooldown
  window (section 3), the new effective voltage is queued and applied
  the moment that cooldown expires, rather than firing an extra PATCH
  outside the normal cooldown-gated cadence.
- Hard safety ceiling: MAX_VCORE_MV, fixed at 1350mV (see section 4).
  This bounds the COMBINATION of ladder step and offset, not either
  value alone - i.e. for every ladder step, coreVoltage + offset must
  not exceed 1350mV. Re-validated on every save that touches either
  the offset or the ladder.
- Validation is performed both client-side (instant feedback, -50/+50
  range check plus the 1350mV combined ceiling check against every
  ladder step) and server-side (authoritative). A save that would push
  any ladder step's effective voltage over 1350mV is refused entirely
  (nothing is written to NVS) with a generic "would exceed max VCore"
  error message - no per-step detail is given.


**14. EDIT BITAXE NAME / IP**
------------------------------
- A small pencil icon appears next to both the name and the IP address
  shown in each device's card on the dashboard, enabling inline
  editing of either field directly from the browser - no reflash
  needed.
- Saved to flash (NVS) across all 16 device slots, survives reboots.
  This means the config array at the top of the sketch only seeds
  initial values on first boot; day-to-day changes live entirely in
  NVS from then on.
- IP address validation: must be a syntactically valid IPv4 address,
  checked both client-side (instant feedback) and server-side
  (authoritative).
- Duplicate IP validation: the new IP is checked against the other 15
  device slots. If it matches any of them, the save is hard-blocked -
  refused entirely, nothing is written to NVS - and an error message
  is shown. (Name has no uniqueness requirement.)
- Editing a device's IP is treated as swapping in a physically
  different unit, exactly like a fresh reconnect (section 3): that
  slot's ladder position, cooldown timer, and minimum-temp/settle
  state are all reset, and the boot-time auto-sync logic (read the
  live BitAxe's actual frequency/voltage, snap to the closest ladder
  position) re-runs immediately against the new address. This avoids
  applying a frequency/voltage calibrated for the old physical unit to
  whatever is now actually at that IP.
- The slot's operation log (section 7) is NOT cleared on an IP or name
  change - it remains a per-slot history. The change itself is written
  into that log as its own entry so the history stays traceable across
  reassignment.
- Editing only the name (IP unchanged) does NOT reset ladder/cooldown/
  settle state, since the physical device targeted hasn't changed.


**15. TESTING**
-----------
Simply flash the program on your ESP32 after setting your WiFi SSID and password in the code:

const char *WIFI_SSID     = "YOUR_WIFI_HERE";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD_HERE";

The program has been tested with the following configuration:
  2x BitAxe Gamma 601
  Meanwell LRS-100 5V 100W
  ESP32 Wroom board powered with 5V (pins 5V and GND)
  Arduino IDE v2.3.8
  	Install Arduino JSON Library (open menu Sketch, Include Libraries, Manage Libraries
  	and write ArduinoJSON. The result should show the version by Benoît Blanchon.
  	Click install.
  	Tested with v7.4.3, should work with versions 6.1+

**COPYRIGHT**
=========   
 *     Author:         Julian E. Spina
 *     Social/contact: jes@acca3.it
 *     Version:        1.0.0 (17 Aug 2026)
 *     License:        MIT
