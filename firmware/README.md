# ESP32 Firmware — Differential-Drive Kart

**ESP-IDF 6.1** firmware (C++) driving **2 independent rear motors** (one per wheel):
**steering is done by the speed difference** between the two wheels (*differential /
skid steer*). The **front wheel is idle**: one free-pivoting 10″ caster, centred. Controlled by a
**Bluetooth gamepad**, speed feedback from **2 AS5600 angle sensors** (one per wheel,
on 2 I²C buses), safety features, a **WS2812B strip** and **Wi-Fi configuration**.

## Mechanical architecture (recap)

```
        FRONT
        🛞               ← 1 IDLE 10″ caster, centred (battery sits over it)
       /  \
      /    \             steering = L/R speed differential
     /      \            (pivots in place if forward ≈ 0)
     [bench]             ← 2 kids, 6″ AHEAD of the driven axle since 2026-08-10
   🛞 L      🛞 R       ← 2 DRIVE wheels, each its own motor + its own AS5600
        REAR              (the CG sits 61 % of the way back from the caster; it
                           was 80 % when the bench sat ON the axle — that move
                           cost a quarter of the rollover margin)
```

- **"Arcade" mixing**: `left = forward + turn·gain`, `right = forward − turn·gain` (stick to
  the left → right wheel faster → the kart turns left). The stick→motor mapping is
  **pluggable** (`mix_type`, Drive feel group): **0 linear**, **1 expo** (gentle around
  center, full authority at the stops — recommended for a child), **2 expo + speed-soft**
  (the accelerating throttle also tapers with measured speed; braking never does).
- **Rollover protection** (a_tip ≈ 0.53 g — ⚠️ NOT a second layer any more, this is what
  keeps the kart upright: with it off the simulation rolls it over): the
  **turn limit follows the measured speed** (iso-lateral-acceleration 1/v curve, ±100% at low speed down to
  `turn_alat_vmax` at the limit) and **reverse is capped**.
  See [Rollover protection](#rollover-protection-turn-too-sharp).

## Build / flash

```bash
. ~/esp/esp-idf-6.1/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

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

- **Custom partition table** ([`partitions.csv`](partitions.csv)): `factory` ~2.75 MB,
  **no OTA** (4 MB flash), plus a dedicated **64 kB `evlog` partition** at 0x2D0000 for the
  persistent event log — appended in the free space AFTER the app, so reflashing the table
  never moves (or wipes) the NVS.
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
[`input.hpp`](main/input.hpp) (`input::get()` → `{x, y, connected, estop}`).

- **Left stick**: `Y` = forward/reverse, `X` = turn (arcade mixing in the controller).
- **Circle→square compensation**: the stick is mechanically bounded by a **circle**
  (`x²+y²≤1`) → at full diagonal each axis would cap at ~0.71. `input::get()` **radially
  stretches** the command (factor `|v|/max(|x|,|y|)`, =√2 on the diagonal) so that the
  **corners of the square become reachable**: full forward **and** full turn simultaneously.
- **B button** = **emergency stop** (immediate braking).
- **Haptic feedback** (`input::rumble`): a **soft** vibration on arming, a **strong** one on
  a sudden error / e-stop, and a **strong (repeated)** one if you push the stick while the
  vehicle is blocked (not armed, etc.). The request is posted by the control loop and
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
both motors into braking mode**. Same if not armed, not calibrated, emergency stop, or
sensor/LVC fault. On top of the BT-level disconnect there is a **heartbeat**: a link still
"connected" but silent for **750 ms** (Wi-Fi/BT coexistence can starve the HID stream for a
few hundred ms — 250 ms false-tripped mid-run) disarms and brakes without waiting for the
multi-second Bluetooth supervision timeout.

### Safety: electrical fault = braking (dead-man)

- **2 s task watchdog with PANIC** (`sdkconfig.defaults`): a frozen control loop → reboot
  (not just a warning); on restart the motors come back up in dynamic braking.
- **Motor pins: all low = braking** (duty 0 + DIR low short the bridge). The
  firmware arms **internal pull-downs** on PWM/DIR (high impedance → braking as long as
  the chip is running) and forces the braking state on the very first line of `app_main`.
- ⚠️ **Wiring required**: the internal pull-downs **do not survive a reset** (IO_MUX
  registers). During the bootloader (~700 ms), only **EXTERNAL pull-downs** (~10 kΩ) on
  the driver's PWM/DIR inputs guarantee braking — plan for them (or check that the
  driver module includes them). The **power latch** (`POWER_HOLD`, active low) is
  deliberately NOT held through a reboot: no capacitor — the logic rail drops, the kart
  powers off cleanly and is re-primed with START (a hidden FORCE ON switch covers bench
  work). A reset on a slope therefore means an unpowered driver = **coasting** — flat
  ground use, as the `coupure_pente*` simulation scenarios quantify.

## Analog measurements (external ADS1115 ADC)

All analog inputs go through an **ADS1115** (16-bit, I²C, PGA) instead of
the ESP32's internal ADC — **more accurate and linear**, and without the ADC2/Wi-Fi conflict.
The breakout is wired **piggyback on I²C bus 0** (alongside the left AS5600: distinct
addresses `0x36` / `0x48`). Dedicated driver: [`ads1115.hpp`](main/ads1115.hpp).

- ⚠️ **Power it at 3.3 V** (ESP32-compatible I²C levels) → `AIN_max = 3.3 V`.
- **A0 = battery voltage** (via the 100k/15k voltage divider), sampled in **continuous
  mode** (±4.096 V, 125 µV resolution). `board::vbatVolts()` reads the conversion register.
- **A1 / A2 = reserved** for the future X/Y joystick (single-shot read); **A3 free**.
- Address set by the ADDR pin (`0x48` GND … `0x4B` SCL) — see `pinout.hpp`.

The driver **degrades gracefully**: if the ADS1115 is absent (not wired), `begin()` detects it
(I²C probe) and `vbatVolts()` returns **−1 = voltage UNKNOWN** — the controller then skips the
LVC and the automatic PWM cap (bench use). Never 0: a 0 V reading is a *valid* voltage to the
caller, and an earlier version returning it made the LVC "see" a flat battery and cut the
power. A chip that goes silent **mid-run** is tolerated for 10 consecutive failed reads
(~0.5 s, holding the last good value) before the voltage is declared unknown, and recovers on
the first good read; its I²C transactions are bounded by the same 2 ms timeout as the
encoders, so a dead sensor can never stall the 500 Hz loop (`doc/audit-2026-07.md`).

### Physical joystick (reserved, not implemented)

The design provides for a **physical joystick** as an alternative to the gamepad. For now
**only Bluetooth is implemented**, but the `input::` interface is neutral and **2 ADS1115
channels (A1/A2) are reserved** ([`pinout.hpp`](main/pinout.hpp), `namespace pins::ads`).

### Wheel encoders

The **2× AS5600** are the whole story — quadrature encoders were once penciled in as a
fallback, but the magnetic sensors do the job and the reservation is gone. GPIO 21/22/23 and
the input-only 34/35/36/39 are free.

## Wireless configuration (SoftAP + WebSocket)

On startup, the ESP32 creates an access point:

- **SSID**: `Kart-Config`  ·  **password**: `kart12345`
- Open **http://kart.local** (mDNS) or **http://192.168.4.1**

The **Wi-Fi** tab lets you enter an SSID/password and **enable station mode**
(checkbox): the kart then connects to that network **while keeping the SoftAP**
(AP+STA mode). Applied **at restart**; automatic reconnection every 5 s.

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
every arm, disarm (with the fault mask of that very tick — the answer to "why did it
stop"), fault raised mid-run, boot and power-off, journaled to the dedicated flash
partition, surviving reboots and power cuts. Live state at 20 Hz (status badge, bars,
I/O dots) + **graduated Chart.js charts** fed by an **in-RAM history**
on the ESP32 side. ⚠️ **While the kart is armed, everything that writes flash is
refused** — Save (config), Save & reboot (Wi-Fi), pairing/unpairing, calibration — a
flash write suspends the cache and would freeze the control loop mid-drive; the page
greys those buttons out with an amber "Kart armed" notice. The **Forward · PWM** chart additionally shows the **speed (rpm) of each wheel
on a 2nd axis** (right). The **Gamepad** tab gathers: a **pairing button**, **gamepad
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
| `pinout.hpp` | **Hardware pinout** (2 motors, 2 I²C buses, joystick/future reserves) |
| `control_types.hpp` | **PURE types shared host/target**: `KartConfig`, state/fault enums, `hw::` constants, `ParamDesc` |
| `config_params.cpp` | **`PARAMS[]` table** (defaults/bounds/help) — PURE, also compiled by the simulation |
| `config.hpp` / `.cpp` | **NVS** persistence (write-if-changed; every save verb is **refused while armed**) + `KartStatus` telemetry + mutex |
| `hardware.hpp` / `.cpp` | Low-level hardware (`board::` — Vbat via ADS1115, **2× PWM/DIR**, **2× AS5600** on 2 I²C buses, buttons, LED, latch, e-stop coil sense, boot-time I²C scan) |
| `ads1115.hpp` / `.cpp` | **ADS1115 driver** (external 16-bit I²C ADC, PGA) — continuous / single-shot modes, 2 ms transaction timeout |
| `input.hpp` / `.cpp` | **Gamepad input** (neutral interface) + **mandatory calibration** (NVS); calibration/pairing refused while armed, collection forces a disarm |
| `input_bp32.c` | **Bluepad32/BTstack backend** (custom platform + BT loop task) |
| `controller_core.hpp` / `.cpp` | **Control CORE (`KartController`, PURE, host-compilable)**: mixing, rollover protection, arming, faults — I/O through injected callbacks (`setCallbacks`), identical on ESP and in the simulator |
| `controller.hpp` / `.cpp` | `EspController`: wires the callbacks onto `board::`/`input::`, host advisors (rumble, power-off), event-log edges, loop-timing telemetry, 500 Hz task |
| `mixer.hpp` | **Pluggable stick→motor mixing** (abstract `Mixer`): linear / expo / expo+speed-soft — safety limits stay outside the mixers |
| `advisors.hpp` | **Host decisions** from telemetry (PURE, shared with the sim): rumble, LVC power-off, idle power-off |
| `evlog.hpp` / `.cpp` | **Persistent event log**: RAM ring pushed by the control task, drained to the `evlog` partition by the LED task **only while disarmed** |
| `pid.hpp` | Reusable **PID** controller with **anti-windup** |
| `ringbuffer.hpp` | Header-only ring buffer (history series) |
| `leds.hpp` / `.cpp` · `ws2812.*` | Status task (WS2812B strip, RMT driver) + event-log drain |
| `mdns_svc.hpp` / `.cpp` | **mDNS** responder (`kart.local`, `_http._tcp`) |
| `webserver.hpp` / `.cpp` | SoftAP + **HTTP/WebSocket** server (protobuf verbs, armed-lockout guards, event-log replies) |
| `assets/` | `index.html` + `style.css` + `chart.min.js` + `pb.min.js` — **gzipped at build** and served with `Content-Encoding: gzip` |
| `main.cpp` | `app_main`: subsystem init + task startup |

### Physics simulation + 3D visualizer

The SAME logic (`controller_core.cpp`) drives a **physics model of the vehicle**
(`test_host/sim/vehicle.hpp`: differential dynamics, DC motors with back-EMF, battery
with internal resistance, simulated sensors, slope, **rollover criterion for 3 OR 4 wheels**
(`VehicleParams::wheels` — the abandoned tricycle is kept so the scenarios can still show why)
through **extreme and realistic scenarios** (`test_host/sim/scenarios.hpp`): full turn
at full speed (with AND without rollover protection — the counter-test tips over), slalom, erratic
driving, stick braking, gamepad loss (heartbeat), encoder failures, LVC, descent…
plus a **parameter sweep** (`turn_hi × turn_full_ms × speed_limit_ms`).

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

1. Reads the gamepad (`input::get()` → `x`, `y`, `connected`, `estop`, `start`).
2. Reads the **2 wheel speeds** (each AS5600, signed 12-bit angle derivative → **m/s**).
   **Vehicle speed = signed average of the two wheels**: two equal wheels in opposite directions
   (pivot in place) → **0 m/s**. This is what feeds the limiter and the telemetry.
3. **Slope limiter on the turn** (`turn_rate` — smooths abrupt steering; the forward command
   is deliberately NOT slewed: acceleration shaping belongs to the mixing curve and the
   control layer), **rollover-protection capping** (can be disabled: `turn_limit_en`, for
   testing), then the selected **mixer** (`mix_type`: linear / expo / expo+speed-soft) maps
   `(forward y, turn x, measured v)` → left / right wheel commands.
4. Per wheel: **braking PID** (brings back to 0 when the command is zero, can be disabled:
   `brk_pid_enable` → dynamic-braking fallback) + **speed-limiter
   PID** (caps the vehicle speed at `speed_limit_ms`, in m/s; can be disabled:
   `vlim_enable`), output **capped**: **automatic 12 V/measured-Vbat**
   cap (12 V motors, 6–30 V driver: 12 V battery → ~100%, 24 V → ~50%)
   AND **manual** cap `duty_cap` — the more restrictive one wins. Without an ADS1115 (Vbat unknown): manual only.
5. **Independent PWM + DIR** to the driver's 2 channels.

`can_drive` requires: gamepad **connected**, **calibrated**, **armed**, no emergency stop,
no fault. Otherwise → **braking of both wheels** (the **default** state, from boot). The
web page shows a **banner clearly listing every blocking reason** (disconnected, not
armed, not calibrated, e-stop, LVC, sensor fault).

Safety features: **arming** by a ~1 s press on START — **physical button OR the gamepad's
START/Options button** (centered + connected gamepad required; starts **disarmed**),
**any fault forces disarming** (you must rearm once it is resolved), **auto disarm**
after inactivity, **emergency stop** (B button → immediate braking), **low-voltage cutoff
(LVC)** with hysteresis (+ latch cutoff), **thresholds hard-coded for the 12 V or
24 V battery detected at startup** (voltage stable 3 s, type frozen until restart) — **disabled if the voltage sensor is
absent** (Vbat < 0 ⇒ we rely on the BMS, useful on the bench without an ADS1115), **encoder
sanity** — **stalled** wheel (PWM without rotation), **reversed** direction (wheel measured
opposite to a clear command — OPTIONAL via `enc_rev_chk`, default on: it can false-trip when
plugging-braking on a downhill, and an owner who verifies the rpm signs at commissioning may
prefer to disable it; the stuck and aberrant nets remain) and **aberrant** measurement
(physically impossible speed) ⇒ **total stop latched until restart** (a lying sensor
would make active (PID) braking and the limiter dangerous), **gamepad heartbeat 750 ms**,
**2 s watchdog with PANIC**, **e-stop coil sense** (GPIO22, always on —
no software bypass; the mushroom becomes a blocking fault with immediate disarm and braking, even against a welded relay contact; bench without the opto: tie GPIO22 to GND), **idle
power-off** (`idle_off_min`: a kart left disarmed powers itself down, countdown on the
Dashboard), **automatically capped PWM** (12 V/measured Vbat), and the **persistent event
log** so a disarm that nobody saw still has its cause on record.

The web page's **Dashboard** tab lists **all active conditions** simultaneously
(the `faults` mask, bits named `fb::` in `control_types.hpp`), with explanation and remedy —
the tab turns red as soon as a serious fault is present. For faults that are already GONE
by the time anyone looks, the **System** tab's event log holds the history.

> **`use_encoders` option (0/1)**: at **0**, the firmware ignores the AS5600s — no speed
> control and no active (PID) braking (it relies on the PWM caps), and **no "stalled
> sensor" fault**. Essential for **testing on the bench without wired encoders** (otherwise the sensor
> fault triggers as soon as you command PWM without measured rotation).

### Rollover protection (turn too sharp)

A tricycle tips about the line from its SINGLE wheel to one of the paired wheels, so the
usable half-track is `(distance CG→single wheel)/wheelbase`. This kart puts the bench 6″
ahead of the PAIRED (driven) axle, which keeps 61 % of it — 254 mm of a 416 mm half-track,
a_tip 5.16 m/s². ⚠️ Since 2026-08-10 that is NOT enough on its own: the counter-test
`virage_sans_protection` (limiter off, full-speed turn) **rolls the kart over** at
−0.60 m/s², where the same run stayed planted at +0.92 while the bench sat on the axle.
`turn_limit_en` is safety-critical firmware now. The turn is shaped on **two fronts**:

1. **Speed→turn ISO-a_lat limit** (`ctl::turnLimit`, tested on the host) — the limit
   follows the **MEASURED vehicle speed** (m/s, signed average of the 2 wheels):
   - `|v| ≤ turn_full_ms` (default 0.5 m/s) — and everywhere the 1/v curve exceeds 100% —
     turn **±100%**: **full-power pivot in place** (`turn_gain` default 1.0)
     stays allowed;
   - beyond that, the limit decreases as **1/v** (same lateral acceleration at any speed)
     down to **`turn_alat_vmax`** (±20%, which is now also its MAXIMUM) at `speed_limit_ms`, then **keeps
     tightening** in case of runaway. Calibrated by simulation: the old linear ramp
     tipped over offset loads as soon as `turn_gain = 1`.
   ⚠️ Relies on the measured speed: with `use_encoders = 0`, v = 0 → no capping.
2. **Sharpness (slope limiter / slew-rate)** — the turn command cannot change
   by more than `turn_rate` units/s: an instantaneous stick jab is **smoothed**. The forward
   command is deliberately **not** slewed — softening the throttle is the job of the
   **expo mixing curves** (`mix_type` 1/2, Drive feel group), which soften the mid-stick
   without robbing the stops, and of the speed limiter.

In addition, **reverse** has **its own speed limit** (`rev_speed_ms`,
default 1 m/s): same PID limiter as forward (`speed_limit_ms`), the target is chosen according
to the **measured direction** — during plugging (stick back, kart still moving forward) the braking
authority stays full. No more dedicated PWM cap. Rollover protection works on |v|:
it bounds the turn in reverse as in forward (the caster simply swivels round).

Web parameters: **`turn_gain`**, **`turn_full_ms`**, **`turn_alat_vmax`**, **`turn_rate`**
(and the Drive feel group: **`mix_type`**, **`mix_expo_fwd`**, **`mix_expo_turn`**,
**`mix_soft_hi`**).

## ⚠️ To adjust before first startup

- **Speed sensors**: kinematics **hardcoded** in `control_types.hpp` (`namespace hw`) —
  `AS5600_CPR = 4096`, `WHEEL_DIAM_M = 0.254` (10″ wheel); the mount-dependent ratio is the
  **`enc_per_wheel`** web parameter (magnet at the gearbox output = 1.28, on the 1:5
  intermediate shaft = 3.41). **2 AS5600**, **one per I²C bus** (fixed address `0x36` → a
  single sensor per bus). To be **verified on the bench**.
- **Encoder signs**: push the kart forward and check both wheel rpm read POSITIVE on the
  Dashboard; fix with `enc_inv_l` / `enc_inv_r` (the two sides are mirrored — one usually
  needs it). Redo this check after ANY motor/sensor rework; it is what lets you disable the
  runtime reversed-encoder watchdog (`enc_rev_chk`) if its downhill-plugging false trip
  bothers you.
- **Battery divider**: the ratio is **fixed by the soldered resistors** — constants
  `hw::VBAT_R_TOP` / `VBAT_R_BOTTOM` in `control_types.hpp` (100k/15k for a 12 V pack).
  Swap the resistors ⇒ edit the two constants and reflash; there is **no web parameter**
  for it, a wrong value would silently drag the LVC thresholds along.
- **Gamepad**: pair (Gamepad tab) then **calibrate** — mandatory to drive.
- **Web settings**: `speed_limit_ms` (m/s), `duty_cap` (manual PWM cap), `turn_gain` /
  `turn_alat_vmax` (rollover protection), `mix_type` (1 or 2 for a child driver). Start
  **wheels up**, low speed.
- **PID**: `vlim_*` (speed limiter) and `brk_*` (braking) — preset
  (limiter ≈ 0.54/0.50, braking ≈ 0.43/0.29/0.011, in m/s), to be **fine-tuned on the bench**.

> Check the **direction of each wheel** (swap the motor wires if needed) and the **direction of the
> differential** (pushing the stick to the right must turn right) **before touching the ground**.
