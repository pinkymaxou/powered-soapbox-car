# ESP32 Firmware — Differential-Drive Kart

**ESP-IDF 6.1** firmware (C++) driving **2 independent rear motors** (one per wheel):
**steering is done by the speed difference** between the two wheels (*differential /
skid steer*). The **front wheel is idle**: one free-pivoting 10″ caster, centred. Controlled by a
**Bluetooth gamepad** (START arms, **A held to drive**), **no sensor on the wheels** (stick →
PWM, bounded by `duty_cap`), safety features, a **WS2812B strip** and **Wi-Fi configuration**.

## Mechanical architecture (recap)

```
        FRONT
        🛞               ← 1 IDLE 10″ caster, centred (battery sits over it)
       /  \
      /    \             steering = L/R speed differential
     /      \            (pivots in place if forward ≈ 0)
     [bench]             ← 2 kids, 6″ AHEAD of the driven axle since 2026-08-10
   🛞 L      🛞 R       ← 2 DRIVE wheels, each its own motor + gearbox + 18T→32T chain
        REAR              (the CG sits 61 % of the way back from the caster; it
                           was 80 % when the bench sat ON the axle — that move
                           cost a quarter of the rollover margin)
```

- **"Arcade" mixing**: `left = forward + turn·gain`, `right = forward − turn·gain` (stick to
  the left → right wheel faster → the kart turns left). The stick→motor mapping is
  **pluggable** (`mix_type`, Drive feel group): **0 linear**, **1 expo** (gentle around
  center, full authority at the stops — recommended for a child).
- **Hold A to drive**: once armed, **A released = dynamic brake** whatever the stick says,
  **A held + stick = drive**, **A held + stick centred = pseudo-freewheel**. See
  [Driving](#driving-a-button-brake-and-pseudo-freewheel).
- ⚠️ **No rollover protection** (a_tip ≈ 0.53 g): nothing measures the speed, so nothing limits
  the turn — `duty_cap` is the only lever. At the default 1.0 the simulation tips the kart in a
  full-speed full turn with an off-centre load. See [Rollover](#rollover-no-protection-the-numbers).

## Build / flash

```bash
. ~/esp/esp-idf-6.1/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Over the air (OTA)

Once a board runs an OTA-capable firmware, no cable is needed (kart **disarmed**):

```bash
curl --data-binary @build/kart_firmware.bin http://kart.local/ota   # or the STA / 192.168.4.1 IP
```

— or the **System** tab of the page (file picker → *Upload & reboot*). The image goes to
the idle slot, is validated, and the board reboots into it. **Rollback** is on: if the new
image crashes before the end of `app_main`, the next reset returns to the previous one. The
upload is refused while armed and aborted if the kart gets armed mid-way (every chunk writes
flash, which freezes the control loop). ⚠️ No authentication: anyone on the kart's network
can flash it — same as the settings.

**Migrating a board from the old `factory` table (one time, by cable):** the partition
table, bootloader (rollback) and evlog location all change, so flash over serial and erase
the evlog's new area (it may hold stale bytes):

```bash
idf.py -p /dev/ttyUSB0 flash
python -m esptool -p /dev/ttyUSB0 erase-region 0x3E0000 0x10000
```

NVS (settings + gamepad pairing) does not move and survives; the old event log is lost.

> **Vendored components** (in [`components/`](components/), **committed** — a fresh clone
> builds without any manual step): `bluepad32`, `btstack`, `cmd_nvs`, `cmd_system`, **patched
> for IDF 6.1**. Provenance and patch details: [`components/README.md`](components/README.md).
>
> **Managed component**: `espressif/mdns` (declared in [`main/idf_component.yml`](main/idf_component.yml))
> is downloaded into `managed_components/` on the first build → **that build needs an internet
> connection**. mDNS was removed from ESP-IDF in v5; the registry is the only source.

### Bluetooth + Wi-Fi: radio configuration

The ESP32 shares a **single radio** between Wi-Fi and Bluetooth (TDM coexistence) and the app
grows significantly (~1.4 MB). Settings in [`sdkconfig.defaults`](sdkconfig.defaults):

- **Custom partition table** ([`partitions.csv`](partitions.csv)): **two OTA slots**
  `ota_0`/`ota_1` of 1.875 MB each (app ~1.5 MB, 4 MB flash), `otadata`, and a dedicated
  **64 kB `evlog` partition** at 0x3E0000 for the persistent event log. The NVS never moves
  (moving it would wipe the settings and the pairing).
- **BT enabled** (`BT_ENABLED`, **BTDM** mode = BLE + BR/EDR, *modem sleep* disabled) +
  **software coexistence** (`ESP_COEX_SW_COEXIST_ENABLE`).
- **Bluepad32**: **CUSTOM** platform (`BLUEPAD32_PLATFORM_CUSTOM`), audio disabled.
- **IRAM**: Wi-Fi + BT saturate the IRAM → `ESP_WIFI_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT`
  disabled (Wi-Fi code moved to flash; negligible impact on control).

## Bluetooth gamepad (Bluepad32)

The backend ([`input_bp32.c`](main/input_bp32.c)) implements a **custom Bluepad32
platform** and runs the **BTstack** loop in a dedicated task (core 0). Gamepad frames
are passed to the firmware through C hooks (`inputbp_on_data` / `inputbp_on_conn`)
consumed by [`input.cpp`](main/input.cpp), which exposes the neutral interface
[`input.hpp`](main/input.hpp) (`input::get()` → `{x, y, connected, estop, start, drive}`).

- **Left stick**: `Y` = forward/reverse, `X` = turn (arcade mixing in the controller).
- **A button** = **hold to drive** (`drive`): Bluepad32's `BUTTON_A`, mask 0x01 — the button
  the page's Gamepad tab lights as "A", whatever is printed on the pad. Released = dynamic brake.
- **START/Options** = arming (held `arm_hold_ms`, stick centred).
- **Circle→square compensation**: the stick is mechanically bounded by a **circle**
  (`x²+y²≤1`) → at full diagonal each axis would cap at ~0.71. `input::get()` **radially
  stretches** the command (factor `|v|/max(|x|,|y|)`, =√2 on the diagonal) so that the
  **corners of the square become reachable**: full forward **and** full turn simultaneously.
- **B button** = **emergency stop** (immediate braking).
- **Haptic feedback** (`input::rumble`): a **soft** vibration on arming, a **strong** one on
  the e-stop, and a **strong (repeated)** one if you push the stick while the kart is **not
  armed** — only then: releasing A while armed is the normal way to brake and does not buzz. The request is posted by the control loop and
  played in the BT thread (`play_dual_rumble`).
- **Pairing / unpairing** driven from the web page (**Gamepad** tab).

### MANDATORY calibration

**The kart refuses to move until the gamepad is calibrated** (`input::get()`
returns zeroed axes if not calibrated → the controller stays braked). Calibration is
done **exclusively from the web page**, **for the gamepad only**:

1. **Center**: stick at rest → captures the neutral point.
2. **Extremes**: move the sticks fully → captures the amplitude per axis.

The scale (center + half-amplitude per axis) is **persisted in NVS** (namespace `pad`).
⚠️ **A (re)pairing ERASES the calibration** (new gamepad = new calibration).

### Safety: disconnect → braking

If the gamepad **disconnects** (out of range, dead battery, unpairing), `connected`
goes to `false`, **every axis and button reads neutral** (a stick frozen mid-deflection or
a latched e-stop bit must not outlive its gamepad), and the controller **immediately puts
both motors into braking mode**. Same if not armed, not calibrated, gamepad emergency stop,
or A released. On top of the BT-level disconnect there is a **heartbeat**: a link still
"connected" but silent for **750 ms** (Wi-Fi/BT coexistence can starve the HID stream for a
few hundred ms — 250 ms false-tripped mid-run) disarms and brakes without waiting for the
multi-second Bluetooth supervision timeout.

### Safety: anything wrong = braking (dead-man)

- **2 s task watchdog with PANIC** (`sdkconfig.defaults`): a frozen control loop → reboot
  (not just a warning); on restart the motors come back up in dynamic braking.
- **Motor pins: all low = braking** (duty 0 + DIR low short the bridge). The
  firmware arms **internal pull-downs** on PWM/DIR (high impedance → braking as long as
  the chip is running) and forces the braking state on the very first line of `app_main`.
- ⚠️ **Wiring required**: the internal pull-downs **do not survive a reset** (IO_MUX
  registers). During the bootloader (~700 ms), only **EXTERNAL pull-downs** (~10 kΩ) on
  the driver's PWM/DIR inputs guarantee braking — plan for them (or check that the
  driver module includes them). There is no power latch any more: the kart is held on by a
  physical main switch, so a reset simply reboots it (~1 s of unpowered driver = **coasting**)
  instead of powering it off. The **emergency stop** costs the same coasting, and for the same
  reason — it opens the main relay and drops the whole kart, ESP32 included (see
  [`../doc/electronique.md`](../doc/electronique.md)). Flat ground use, as the
  `coupure_pente*` and `arret_urgence_plat` simulation scenarios quantify (~11 m of coasting
  from full speed on the flat).

## No analog measurements at all

There is **no ADC on this kart** — no ADS1115, no internal ADC, no voltage divider. The pack
is always **12 V**, which is the motors' nominal voltage, so there is nothing to cap
automatically and nothing worth measuring. What went away with it: the low-voltage cutoff
(LVC), the 12/24 V detection at boot, the automatic `12 V / measured Vbat` PWM cap, the
battery gauge and chart on the page, and the `LVC` / `NO_VBAT` faults.

What remains as a power ceiling: the manual **`duty_cap`** setting. What remains as
deep-discharge protection: the battery's own. A flat pack is now something the driver
*feels* — the kart is simply slower, as the `batterie_usee` simulation scenario shows — not
something the kart reports.

### Physical joystick (reserved, not implemented)

The design provides for a **physical joystick** as an alternative to the gamepad. For now
**only Bluetooth is implemented**; the `input::` interface stays neutral so one could be added
behind it, but the two ADS1115 channels that used to be reserved for it went away with the
converter — a physical joystick would now need an ADC of its own.

### No wheel sensors

The **2× AS5600** encoders and their two I²C buses were removed on **2026-09-29**. Nothing
measures the wheels: no speed, no rpm, no encoder conditions (stuck / reversed / erratic /
absent / magnet — their bits and protobuf values are **retired, not reused**, so older
event-log records still decode to what they meant). Gone with them: the speed-limiter and braking PIDs
(`pid.hpp`), the rollover protection, the turn slew-rate, the speed-soft mixer (`mix_type` 2),
the reverse speed limit, and the settings `use_encoders`, `open_loop`, `mix_soft_hi`, `enc_per_wheel`,
`enc_inv_l/r`, `enc_rev_chk`, `speed_limit_ms`, `rev_speed_ms`, `brk_*`, `vlim_*`,
`turn_limit_en`, `turn_full_ms`, `turn_alat_vmax`, `turn_rate`, `dyn_brake_en`.

Free GPIOs: **13, 14, 16, 18, 19, 21, 22, 23, 27** and the input-only 34/35/36/39 — 13 and 22
were the power latch and the e-stop coil sense, 16 the arming button, 18/19 and 27/14 the two
I²C buses.

## Wireless configuration (SoftAP + WebSocket)

On startup, the ESP32 creates an access point:

- **SSID**: `Kart-Config`  ·  **password**: `kart12345`
- Open **http://kart.local** (mDNS) or **http://192.168.4.1**

The **Wi-Fi** tab lets you enter an SSID/password and **enable station mode**
(checkbox): the kart then connects to that network **while keeping the SoftAP**
(AP+STA mode). Applied **at restart**. Automatic reconnection with a **growing delay** (5 s after a drop,
then 10, 20, 40 s… up to 5 min; back to 5 s once connected) and **never while armed**: an
attempt scans every Wi-Fi channel on the radio the gamepad's Bluetooth shares.

### mDNS (`kart.local`)

The kart announces itself on the local network via **mDNS/Bonjour**
([`main/mdns_svc.cpp`](main/mdns_svc.cpp), managed component `espressif/mdns`), on **both
interfaces** (SoftAP *and* station): the same URL **http://kart.local** works whether you are
connected to `Kart-Config` or to the home network — no need to know the DHCP address, which
changes. The `_http._tcp` service is also published, so the kart appears on its own in the
network browsers (Bonjour, `avahi-browse`, Windows Explorer "Network"). The host name is
**also sent to the router's DHCP server** (`esp_netif_set_hostname`), so the kart shows up as
`kart` in the list of connected clients.

- Name defined in one place: `HOSTNAME` in [`main/mdns_svc.cpp`](main/mdns_svc.cpp); it is
  shown in the **System** tab and used for the DHCP host name.
- Supported out of the box on **Windows 10+, macOS/iOS and Linux** (Avahi).
  ⚠️ **Android**: Chrome does *not* resolve `.local` → use `http://192.168.4.1` from a phone.
- If two karts are on the same network, the library detects the collision and appends a
  number (`kart-2.local`).
- Cost: ~34 kB of flash and one low-priority task; a failure to start is **logged and
  ignored** — access by IP is never affected.

The page (7 tabs: **Dashboard / Graph / Configuration / Gamepad / Wi-Fi /
Documentation / System** — the pinout lives in the Documentation tab) communicates over **WebSocket** (`/ws`) using **binary Protocol Buffers** — a single
schema [`main/proto/kart.proto`](main/proto/kart.proto) (regenerate: `main/proto/generate.sh`),
encoded on the kart side by **nanopb** (vendored, callbacks → zero-copy/zero-heap, from the
static arena) and decoded browser-side by **protobuf.js** (`/pb.js` embedded, mirror JSON
descriptor in the page). Frames are ~3–10× smaller than the old JSON (status ≈ 150 B, full hist
≈ 0.9 kB). Whatever is **immutable at runtime is sent once when the page opens**:
config metadata ("get"), system info ("sysinfo"). After that: "vals" (values
only) after save/reload, "sysdyn" (uptime/heap/worst-tick) when the tab is shown,
charts ("hist") every 5 s, and the **persistent event log** ("evlog", System tab):
every arm, disarm (with the `Stop` cause of that very tick — the answer to "why did it
stop") and boot, journaled to the dedicated flash partition,
surviving reboots and power cuts. (The two power-off codes are retired with the power latch:
the firmware cannot cut its own supply any more, so a power loss now leaves no record —
the next Boot entry is what marks it.) Live state at **up to** 20 Hz — the page asks for the
next status only once the previous one has arrived, so a busy radio (a station retry scans
every channel) slows the refresh down instead of filling the socket with a backlog the page
then replays in one burst. Status badge, bars and I/O dots, plus **graduated Chart.js
charts** fed by an **in-RAM history**
on the ESP32 side. ⚠️ **While the kart is armed, everything that writes flash is
refused** — Save (config), Save & reboot (Wi-Fi), pairing/unpairing, calibration — a
flash write suspends the cache and would freeze the control loop mid-drive; the page
greys those buttons out with an amber "Kart armed" notice. The charts show the forward
command, the PWM of each wheel and the worst control tick — no speed or rpm, nothing measures
them. The **Gamepad** tab gathers: a **pairing button**, **gamepad
info** (name, battery, connection), an **unpairing button**, the **calibration mode**,
and a **real-time visualization**: a 2D pad showing **two points** — the **physical
position** of the left stick (blue, on the circle) and the **compensated command** circle→square
(orange, reaching the corners of the square), joined by a line — a 2nd pad for the **right stick**
(display only, not calibrated), the **directional cross (D-pad)**, the **button-state
dots**, the **ZL/ZR trigger bars**, and the **raw (hex) button mask** (to
identify the specific buttons of a gamepad).

## Software architecture

| File | Role |
|---|---|
| `pinout.hpp` | **Hardware pinout** (2 motors PWM/DIR, board LED, WS2812 — and the list of free GPIOs) |
| `control_types.hpp` | **PURE types shared host/target**: `KartConfig`, the `State`/`Stop`/`BrakeMode` enums, `hw::` constants, `ParamDesc` |
| `config_params.cpp` | **`PARAMS[]` table** (defaults/bounds/help) — PURE, also compiled by the simulation |
| `config.hpp` / `.cpp` | **NVS** persistence (write-if-changed; every save verb is **refused while armed**) + `KartStatus` telemetry + mutex |
| `hardware.hpp` / `.cpp` | Low-level hardware (`board::` — **2× PWM/DIR**, dynamic brake, LED). No inputs and no sensors: nothing is wired to a GPIO but the motors and the strip |
| `input.hpp` / `.cpp` | **Gamepad input** (neutral interface) + **mandatory calibration** (NVS); calibration/pairing refused while armed, collection forces a disarm |
| `input_bp32.c` | **Bluepad32/BTstack backend** (custom platform + BT loop task) |
| `controller_core.hpp` / `.cpp` | **Control CORE (`KartController`, PURE, host-compilable)**: A-button gate, mixing, accel ramp, pseudo-freewheel, arming, the `Stop` cause — output through an injected callback (`setCallbacks`), identical on ESP and in the simulator |
| `controller.hpp` / `.cpp` | `EspController`: wires the callback onto `board::`/`input::`, host advisor (rumble), event-log edges, loop-timing telemetry, 500 Hz task |
| `mixer.hpp` | **Pluggable stick→motor mixing** (abstract `Mixer`): linear / expo — `duty_cap` stays outside the mixers |
| `advisors.hpp` | **Host decision** from telemetry (PURE, shared with the sim): gamepad rumble |
| `evlog.hpp` / `.cpp` | **Persistent event log**: RAM ring pushed by the control task, drained to the `evlog` partition by the LED task **only while disarmed** |
| `ringbuffer.hpp` | Header-only ring buffer (history series) |
| `leds.hpp` / `.cpp` · `ws2812.*` | Status task (WS2812B strip, RMT driver) + event-log drain |
| `mdns_svc.hpp` / `.cpp` | **mDNS** responder (`kart.local`, `_http._tcp`) |
| `webserver.hpp` / `.cpp` | SoftAP + **HTTP/WebSocket** server (protobuf verbs, armed-lockout guards, event-log replies) |
| `assets/` | `index.html` + `style.css` + `chart.min.js` + `pb.min.js` — **gzipped at build** and served with `Content-Encoding: gzip` |
| `main.cpp` | `app_main`: subsystem init + task startup |

### Physics simulation + 3D visualizer

The SAME logic (`controller_core.cpp`) drives a **physics model of the vehicle**
(`test_host/sim/vehicle.hpp`: differential dynamics, DC motors with back-EMF, the 1:23.70
drivetrain, battery with internal resistance, slope, **rollover criterion for 3 OR 4 wheels**
(`VehicleParams::wheels` — the abandoned tricycle is kept so the scenarios can still show why)
through **20 extreme and realistic scenarios** (list: `test_host/sim/scenarios.hpp`): full turn
at full speed (no protection — off-centre loads tip at `duty_cap` 1.0), the old layout, pivot,
slalom, erratic driving, plugging, reverse, the A button (`sans_bouton_A`, `relache_A`,
`roue_libre_A`), gamepad loss (heartbeat), e-stop coasting, worn battery, descents on the
dynamic brake, power cut on a slope… plus a **`duty_cap` sweep** (full-speed full turn × three
loads) and the core output tests. The rollover runs are **measurements pinned as asserts**: they
document the danger and keep the README's numbers honest (a change that moves them fails the
build) — they do not block the tipping default.

- **Automated tests**: `test_host/run_tests.sh` (run by CI). CSV trace:
  `KART_SIM_TRACE=trace.csv ./sim`.
- **Real-time 3D visualizer**: `python3 tools/sim_viewer.py` → http://localhost:8650/ —
  same scenarios, 3D view (chase camera, trail, roll proportional to a_lat,
  ROLLOVER alert), margin gauge and live status badges.
- The ESTIMATED physical parameters (Ra, Iz, h_cg, x_cg, frictions) are grouped and
  commented in `VehicleParams` — to be recalibrated with real measurements.

Tasks: **`control`** (core 1, 500 Hz, 2 s watchdog with PANIC), **`leds`** (core 0, ~20 Hz,
also drains the event log) and the **BTstack loop** (core 0). Sharing of `g_cfg` and
`g_status` (mutexes); gamepad state passes through the atomics in `input.cpp`. FreeRTOS runs
at **1000 Hz**.
Priorities / cores / stacks: [`main/rtos.hpp`](main/rtos.hpp) · [`../doc/firmware-tasks.md`](../doc/firmware-tasks.md).

## Control loop (500 Hz)

1. Reads the gamepad (`input::get()` → `x`, `y`, `connected`, `estop`, `start`, `drive`).
   That is the only input: **no sensor is read** — no I²C, no ADC.
2. **Safety gate**: disconnected, silent for 750 ms (heartbeat), B pressed or not calibrated →
   disarm + dynamic brake. START held `arm_hold_ms` with the stick centred arms.
3. Armed: **A released** → dynamic brake, whatever the stick says. **A held + stick** (outside
   `thr_deadzone`) → the selected **mixer** (`mix_type`: 0 linear / 1 expo) maps
   `(forward y, turn x)` → left / right wheel commands. **A held + stick centred** →
   pseudo-freewheel: the last command runs down toward 0 (see [Driving](#driving-a-button-brake-and-pseudo-freewheel)).
4. Output **capped** by the manual `duty_cap` — the single ceiling on everything — then
   `mot_inv_l` / `mot_inv_r` / `mot_swap_lr` applied, and **independent PWM + DIR** to the
   driver's 2 channels.

`can_drive` requires: gamepad **connected**, **calibrated**, **armed**, no emergency stop —
and the kart only moves while **A is held**. Otherwise → **braking of both wheels** (the
**default** state, from boot). The web page **names the reason** it will not drive
(disconnected, silent, gamepad e-stop, not calibrated) — one at a time, highest priority first.

Safety features: **arming** by a ~1 s press on the gamepad's **START/Options button** — the
only arming input there is, now that the board has no GPIO inputs left (centered stick +
connected, calibrated gamepad required; starts **disarmed**),
**anything in the way forces disarming** (you must rearm once it is resolved), **hold-to-drive A**
(released = brake), **auto disarm** after `disarm_s` without driving (A held + stick),
**gamepad emergency stop** (B button → immediate braking), **gamepad heartbeat 750 ms**,
**2 s watchdog with PANIC**, and the **persistent event log** so a disarm that nobody saw
still has its cause on record.

The **hardware emergency stop is not in that list, by construction**: the mushroom sits in
the main relay's coil loop and cuts the ESP32 along with the motors, so the firmware never
sees it, reports nothing and brakes nothing — the kart coasts (~11 m from full speed on the
flat, measured in simulation). Power returns on the main switch and the kart boots
**disarmed**, which is what the old "re-arm after an e-stop" rule enforced in software.

The web page's **Dashboard** tab names **why the kart will not drive** (`enum class Stop` in
`control_types.hpp` — one cause, highest priority first), with explanation and remedy, and the
tab turns red while there is one. For a cause already GONE by the time anyone looks, the
**System** tab's event log holds the history.

There is no **fault** layer any more (removed 2026-09-30): no mask, no `Fault` enum, no
`State::Fault`, no red "fault" state on the page. Every sensor that could report a breakage
left one after the other — the battery ADC, the relay-coil sense, the encoders — and what
remained were four ordinary gamepad conditions, none of which is a failure. They still disarm
and brake; they are simply named for what they are.

### Driving: A button, brake and pseudo-freewheel

Once armed, the A button decides the output mode (`BrakeMode` in `control_types.hpp`, shown
permanently on the page):

| Input | Mode | What the driver's outputs do |
|---|---|---|
| **A released** (any stick) | `DYNAMIC` | PWM 0 + DIR low on both channels → both motor terminals grounded: **short-circuit brake** |
| **A held + stick** | `NONE` | the mixed stick, capped by `duty_cap`, rising no faster than `accel_pct_s` and falling no faster than `decel_pct_s` |
| **A held + stick centred** | `COAST` | the target becomes 0 and the command **slides there at `decel_pct_s`** (100 %/s by default = one second from full). Once it arrives, the mode becomes `DYNAMIC` — at 0 the windings are grounded, so calling it a coast would be a lie |

Disarmed, not calibrated, gamepad lost or silent, B pressed → `DYNAMIC` too; that is the
default state from boot.

**Why "pseudo".** The Cytron MDD20A has **no coast / high-impedance state** in PWM/DIR mode:
its truth table ([manual](../doc/datasheet/motor-driver-20A-manual.pdf), Table 3, printed
p. 3) has PWM low → MxA and MxB both low → **Brake**, whatever DIR. So a real freewheel is not
available, and the firmware imitates one: a centred stick targets 0 and the command **slides**
there rather than dropping, so the motor neither pushes nor brakes much on the way.

That slide has no setting of its own — it is simply `decel_pct_s`, the falling half of the
ramp pair. It used to be a duration (`coast_s`, then `coast_ms`), until the obvious was pointed
out on the bench: a run-down *is* a deceleration rate, and having one slope in %/s and the other
in milliseconds was the same idea told twice. At the 100 %/s default (one second from full
PWM) ~1.88 m/s is left half a second after centring the stick, against ~1.57 m/s half a second
after releasing A (`relache_A`) — the numbers the old 1000 ms produced, unchanged. The earlier
10 s, the pace of a true coast measured against `arret_urgence_plat`, was far longer than any
driver expects between centring the stick and stopping. Once at 0 it is electrically the brake
and the mode says `DYNAMIC`. Tuned for the flat: nothing measures a slope. ⚠️ The old
`dyn_brake_en = 0` "freewheel" option **never freewheeled on this driver** — duty 0 is the same
PWM-low state, i.e. it braked.

### Rollover: no protection, the numbers

A tricycle tips about the line from its SINGLE wheel to one of the paired wheels, so the
usable half-track is `(distance CG→single wheel)/wheelbase`. This kart puts the bench 6″
ahead of the PAIRED (driven) axle, which keeps 61 % of it — 254 mm of a 416 mm half-track,
a_tip 5.16 m/s² (0.53 g). **Nothing in the firmware limits the turn** since 2026-09-29 — the
owner's decision, taken when the wheel sensors went (a speed-dependent turn limit needs a
measured speed). `duty_cap` is the only lever. Full-speed full turn, 18T gearing, minimum tip
margin in m/s² (`testCapSweep` in [`test_host/sim_main.cpp`](test_host/sim_main.cpp)):

| `duty_cap` | 2 children centred | 1 child off-centre (33 kg) | adult + child |
|---|---:|---:|---:|
| **1.0** (default) | +1.62 | **−0.24 TIPS** | **−0.07 TIPS** |
| 0.9 | +2.29 | +0.44 | +0.59 |
| 0.8 | +2.88 | +1.07 | +1.19 |
| 0.7 | +3.42 | +1.61 | +1.73 |
| 0.6 | +3.89 | +2.08 | +2.20 |

The default stays 1.0 (the owner's call). A flat-ground test with 40 lb pellet bags could not
tip it — the bags sit lower than a seated child, and the model has never been calibrated
against a real tip test. The sweep asserts that 0.9 and below keep every load upright with
more than +0.3 m/s² to spare; it does not stop anyone from driving at 1.0. **Reverse** is as fast as forward
(`marche_arriere`: −2.56 m/s) — there is no separate reverse limit any more.

Web parameters: **`duty_cap`**, **`turn_gain`** (and the Drive feel group: **`mix_type`**,
**`mix_expo_fwd`**, **`mix_expo_turn`**, **`accel_pct_s`**, **`decel_pct_s`**).

### The two ramps (`accel_pct_s`, `decel_pct_s`)

The stick sets a **target**; the ramps bound how fast the command may travel to it, both in
**% of full duty per second** (`ctl::ramp`, control_math.hpp):

| | Default | What it is for |
|---|---|---|
| `accel_pct_s` — **rise**, away from 0 | 200 %/s (half a second to full power) | the **motor drivers**: a stick slammed open is a step from 0 to full duty, and a brushed motor answers a step like that with its stall current |
| `decel_pct_s` — **fall**, back toward 0 | 100 %/s (one second from full power) | the **pseudo-freewheel**, and smoothing a stick pulled back |

0 disables either side — the command then jumps straight to the target that way. A **reversal**
is the two in sequence: down to 0 at the fall rate, then up the other way at the rise rate (a
tick that finishes the fall spends what is left of itself building the other way, so plugging
never stalls at zero).

Both rates are in **real duty**, hence the division by `duty_cap` in the core: lowering the cap
makes the kart slower without secretly retuning the ramps, so "one second from full power"
stays one second.

⚠️ Neither ramp can delay the **brake**. Releasing A, disarming, B, a lost or silent gamepad —
every one of those grounds the windings on the tick it happens, whatever the ramps are doing.
The ramps only shape a command the driver is still asking for.

## ⚠️ To adjust before first startup

- **Motor directions**: wheels in the air, **A held**, forward stick — both wheels must turn
  forward; fix with `mot_inv_l` / `mot_inv_r` (or swap the motor leads). Then a turn: stick
  left must speed up the RIGHT wheel — if the steering is mirrored, the motors are on the
  wrong driver channels: rewire them or set `mot_swap_lr`. Then release A: both wheels brake.
- **Battery**: nothing to set up — always 12 V, never measured. Charge it on a schedule;
  the kart will not warn you.
- **Gamepad**: pair (Gamepad tab) then **calibrate** — mandatory to drive. Check which button
  the tab lights as **A**.
- **Web settings**: `duty_cap` (the only limit — read the [rollover table](#rollover-no-protection-the-numbers)
  first), `turn_gain`, `mix_type` (1 for a child driver), `accel_pct_s`, `decel_pct_s`. Start **wheels up**, low
  `duty_cap`.

> Check the **direction of each wheel** (swap the motor wires if needed) and the **direction of the
> differential** (pushing the stick to the right must turn right) **before touching the ground**.
