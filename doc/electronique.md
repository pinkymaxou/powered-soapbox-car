# Electronics — design, BOM and wiring guide

The electronics chapter of the kart, in the same spirit as [`reducteur.md`](reducteur.md) for
the drivetrain: every choice with its **why**, the verified parts list, and the
terminal-by-terminal wiring guide. The firmware side of every signal named here is described
in [`../firmware/README.md`](../firmware/README.md).

**Design inputs (fixed):** single **12 V** motorcycle battery (no other voltages this phase) ·
**5 V rail ≥ 2 A** (it powers everything: ESP32, WS2812 strip, sensors) · the low-power relay
feeds **everything except motor power** · the **e-stop cuts motor power only** and an opto
tells the firmware whether the motor rail is live · priming by a **momentary button**, hold by
**GPIO13** (the ESP can power itself off) · **no hold capacitor** · ESP32, ADS1115 and
2× AS5600 confirmed.

![Two-rail power](schematics/power_rails.png)

> Regenerate: `. .venv-schem/bin/activate && python doc/schematics/power_rails.py`
> (environment: [`schematics/requirements.txt`](schematics/requirements.txt)). The whole-kart
> schematic is [`schematics/full_schematic.png`](schematics/full_schematic.png).

## 1. Architecture — two rails, two relays

| Rail | Switched by | Feeds | Dies when |
|---|---|---|---|
| **+12V_LOG** (logic) | small **opto relay module** (COM/NO from the fused battery +) | ESP32 (via buck 5 V), motor-driver **logic**, voltage divider + ADS1115, AS5600 ×2, WS2812 | the ESP releases `POWER_HOLD` (self power-off), or a reboot |
| **+12V_MOT** (motors) | **40 A automotive relay** — its **coil** runs +12V_LOG → **E-STOP (NC)** → 85; the CONTACT (30→87) is what carries and breaks the 40 A | motor driver **VB+** only | e-stop pressed (coil broken → relay opens in ~10-20 ms), or the logic rail dies |

Why the split: the emergency stop must remove **drive power** without killing the brain — the
logic survives, the firmware sees the motor rail die through the sense opto (GPIO22), disarms,
**names the fault** (`MOTOR POWER`) on the page and in the event log, and requires a deliberate
re-arm. Releasing the mushroom never resumes drive on its own.

### Start / hold / self-off (no hold capacitor)

- **Priming**: the momentary **START button** connects the fused battery + straight to the
  logic rail, in parallel with the module's contact — the ESP does not exist yet, something
  physical has to close the loop. Hold it ~1 s: that is the ESP's boot time until `app_main`
  drives `POWER_HOLD`.
- **Hold**: `POWER_HOLD` (GPIO13, **active low**) lights the module's opto input; the module's
  relay closes and takes over from the button. The module's own VCC hangs on the logic rail —
  zero standing drain with the kart off.
- **Self power-off**: the firmware releases the pin after `idle_off_min` minutes disarmed, or
  30 s of LVC — the rail drops, the kart is off. (`board::powerOff()`.)
- **Reboot = clean power-down, by design.** A watchdog reset leaves `POWER_HOLD` undriven for
  ~700 ms; with no capacitor the rail drops and the kart turns off. That is a SAFE outcome
  (the motors are unpowered; on the flat the un-back-drivable 1:17 gearbox stops the kart),
  and it costs one press of START. The old plan held the rail up with ~1500 µF — rejected:
  it also **delays the e-stop's effect on anything wired in the coil path**, and it papers
  over reboots instead of surfacing them (the event log records the boot and its reason).
- **FORCE ON switch (hidden)**: an ordinary toggle **inside the electronics enclosure**, in
  parallel with the button — forces the logic rail permanently on for bench work, flashing
  and diagnosis. Not reachable from the driver's seat, so it cannot defeat the self-off in
  normal use. If it is left on by mistake, the idle power-off fires anyway, the rail stays up
  (the switch holds it), and the firmware simply stays alive with the countdown at 0 — the
  designed behavior for "power that refuses to die".

### The 40 A relay and the e-stop

- Motor current flows **30 → 87 (NO)**: the contact carrying the 40 A rating, and an
  unpowered relay is an unpowered kart. **87a stays spare.**
- **Coil (85/86) fed from the logic rail**: logic off ⇒ motors off, no separate path to keep
  the motor rail alive. **1N4007 flyback across the coil** (cathode to +): the little module's
  10 A contacts switch a 150 mA inductive load and must never break an arc. *(Design-review
  addition — the module protects its own relay, nothing protected this one.)*
- **E-stop (mushroom, NC) in the COIL loop**: +12V_LOG → mushroom → 85. The button switches
  **~150 mA on thin wires** — any decent NC mushroom qualifies, and the 40 A power run stays
  in the nose instead of detouring to the seatback and back in 10 AWG. The **relay contact**
  is what breaks the 40 A. With no hold capacitor anywhere, drop-out is the relay's own
  ~10–20 ms — instant to a human. *(This placement was chosen once the capacitor was
  dropped: the old reason to put the mushroom in the 40 A path was precisely that a charged
  capacitor kept the coil alive ~0.9 s against it.)*
- **Sense opto — on the COIL, after the mushroom** (owner's decision, 2026-08-03): input
  LED through **4.7 kΩ ¼ W** from the coil node (85) to ground. Coil dead = e-stop engaged
  (or logic rail down) → the firmware disarms and **dynamic-brakes**. The point of sensing
  the coil rather than the relay output: **a welded relay contact no longer defeats the
  e-stop** — the software sees the command and stops the kart even though VB+ is still
  there (and braking works all the better for it). The trade, stated plainly: a welded
  contact becomes **invisible** (the sense can no longer prove the contact opened), and a
  relay that fails to CLOSE is not seen here either — that case is caught by the
  wheel-stuck fault (commanded PWM, no rotation, 1 s) and by the obvious symptom. Sizing: R = (V − Vf)/If with
  Vf ≈ 1.2 V gives 2.0–2.9 mA over the whole 10.5–14.8 V battery range (39 mW worst in the
  resistor); the transistor only has to sink the GPIO's internal pull-up (~73 µA), so even
  a minimum-CTR PC817 at the LVC floor keeps a ×9 margin. A ready-made opto module rated
  12 V needs nothing added; one rated 3.3/5 V gets ~1–1.5 kΩ in series with its input. Output transistor between **GPIO22**
  and GND; the pin idles on the ESP32's internal pull-up (that is why it is GPIO22 — the
  input-only 34–39 have no pull hardware) and the firmware debounces 50 ms, so a spike
  coupled from the neighbouring 40 A cabling cannot fake an emergency stop.
- **Contact welding** happens at CLOSING: the contacts bounce for a few ms, and each bounce
  arcs across whatever inrush is waiting — here, the driver's bulk capacitors. Two facts
  follow. First, **driving style is irrelevant to welding**: the relay always closes with
  the kart disarmed and the motors commanded off (boot sequence), so the make-current is
  the capacitor charge, never the motors — gentler mixing curves protect the CHILD, not
  the relay. Second, the fix if it ever welds is a **pre-charge resistor** across the
  contact, not softer throttle. What softer driving DOES reduce is contact **erosion** when
  the e-stop opens the relay under load (the relay then breaks whatever the motors draw,
  up to ~40 A at 12 VDC — inside an automotive relay's normal duty). With the coil-side
  sense, a welded contact is mitigated in software (disarm + dynamic brake, see above)
  but NOT detected — the pre-drive e-stop test verifies the sense chain, no longer the
  contact itself.
- **The relay never closes under motor load — and the DETECTION is what guarantees it.**
  The chain: the e-stop is sensed → blocking fault → forced disarm → releasing the mushroom
  changes nothing until the **full re-arm sequence** (deliberate START hold, stick centered)
  is performed — so at the instant the coil re-energizes and the contact closes, no current
  is being demanded. Same story at power-up (boot forces PWM low before anything else) and
  under FORCE ON. The residual make-current is the driver's capacitor inrush alone, which
  is what justifies shipping without a pre-charge resistor.
  ⚠️ The chain starts at DETECTION, so it stands on `pwr_sense_en = 1`: with the sense
  unwired the firmware never sees the e-stop, a held throttle stays armed through it, and
  releasing the mushroom closes the relay into full motor demand — the worst welding case.
  Wiring the opto is part of the relay's protection, not optional diagnostics.

### 5 V rail — the ≥ 2 A budget

| Consumer | Worst case | Note |
|---|---:|---|
| WS2812 ×10 | ~0.60 A | 60 mA/LED full white; status colors at brightness 64 draw far less |
| ESP32-WROOM (via its 3.3 V LDO) | ~0.70 A | Wi-Fi TX bursts; sustained is ~0.24 A |
| AS5600 ×2 + ADS1115 + opto LEDs | ~0.05 A | on 3.3 V, through the ESP board's regulator |
| Motor-driver logic inputs | ~0.02 A | PWM/DIR are 3.3 V signals; board logic is on +12V_LOG |
| Margin / future (buzzer, lights) | ~0.6 A | |
| **Total** | **≈ 2.0 A** | **buck must sustain 2 A continuous at 11–14.8 V in** |

The on-hand buck is a 20 V-rated module reused at 12 V input — **verify its continuous
rating** (label/heatsink): if it is not clearly ≥ 2 A (3 A class recommended), replace it.
At the WS2812 strip head: the classic **470–1000 µF electrolytic across 5 V/GND** and
**~330 Ω in series with DIN** (first-LED protection), data wire kept short.

### Battery measurement (12 V only)

Divider **100 k / 15 k** from **+12V_LOG** to A0 of the ADS1115 (14.8 V charging → 1.93 V at
A0, comfortably under the 3.3 V rail). The ratio is the compile-time constant pair
`hw::VBAT_R_TOP`/`VBAT_R_BOTTOM` — deliberately not a web setting. On the **logic** rail, not
the motor rail: on the motor side it would read 0 V the instant the e-stop is pressed and the
kart would cry "flat battery" instead of "emergency stop". LVC thresholds (12 V lead-acid):
warn 11.5 V · cut 10.5 V · re-arm 12.0 V, judged on a 2 s-smoothed reading so the ~2 V sag of
a 40 A acceleration cannot false-trip. Decoupling capacitor (100 nF) at A0.

### Protections

| Risk | Measure |
|---|---|
| Short / overload | **40 A blade fuse** in a holder, as close to the battery + as possible — everything downstream is protected, including both relays |
| Reversed driver supply (destroys the board) | **Polarized connectors + color discipline** (red/+, black/−) + the multimeter checklist below. A series diode at 40 A would burn ~30 W; a P-FET adds failure modes to guard a one-time assembly mistake — process beats components here |
| 40 A coil kickback | **1N4007 across 85/86** (see above) |
| Coupled noise on the sense line | internal pull-up + **50 ms firmware debounce** (in `controller_core.cpp`) |
| ESP32 brownout during motor surges | ESP on its own rail (the buck input never sees the motor cables' IR drop); bulk electrolytic (≥ 470 µF) at the buck input |

## 2. BOM (electronics)

| # | Part | Spec / rating | Qty | Role |
|--:|---|---|--:|---|
| 1 | Motorcycle battery | 12 V lead-acid, ≥ 40 A peak | 1 | single pack, centered in the nose |
| 2 | Blade fuse + holder | **40 A** | 1 | master protection at battery + |
| 3 | Opto relay module | 12 V coil, opto input, low trigger, contacts ≥ 10 A | 1 | logic-rail switch (COM/NO) |
| 4 | Automotive relay | 12 V coil, **40 A** on 87 (NO), SPDT | 1 | motor-rail switch |
| 5 | Diode 1N4007 | 1 A / 1000 V | 1 | flyback across the 40 A coil |
| 6 | E-stop mushroom | NC, latching (coil current only: ≥ 1 A) | 1 | in the 40 A relay's coil loop, top of seatback — thin wires |
| 7 | Momentary button | NO, panel mount | 1 | START / priming (GPIO16 side too) |
| 8 | Toggle switch | ≥ 3 A, any | 1 | hidden FORCE ON, inside the enclosure |
| 9 | Buck converter | 12 V in → **5 V ≥ 2 A cont.** (3 A class) | 1 | logic 5 V rail |
| 10 | ESP32-WROOM board | dual-core, 4 MB | 1 | controller |
| 11 | Motor driver | dual channel, 20 A/ch, 6–30 V, PWM+DIR | 1 | both front motors |
| 12 | ADS1115 breakout | 16-bit I²C ADC, addr 0x48 | 1 | Vbat on A0 (bus 0, 3.3 V) |
| 13 | AS5600 breakout + diametric magnet | 12-bit angle, I²C 0x36 | 2 | one per wheel, one per bus |
| 14 | Sense optocoupler (or opto module) | PC817-class; input via **4.7 kΩ ¼ W** | 1 | motor-rail presence → GPIO22 |
| 15 | Resistors 100 k + 15 k | 1 %, ¼ W | 1+1 | Vbat divider (**matches `hw::VBAT_R_*`**) |
| 16 | Resistors 4.7 k | ¼ W | 4 | I²C pull-ups (2 per bus) |
| 17 | Resistor ~330 Ω | ¼ W | 1 | WS2812 DIN series |
| 18 | Capacitors: ≥ 470 µF (buck in), 470–1000 µF (LED strip), 100 nF (A0) | 16 V+ | 3 | bulk + decoupling |
| 19 | WS2812B strip | ~10 LEDs, 5 V | 1 | status display (GPIO4) |
| 20 | Enclosure ~150×100×70, clear lid | ≈ IP65 + cable glands | 1 | ESP32 + breakout + ADS1115 + perfboard |
| 21 | Wire: 10 AWG (power), 18–22 AWG (signal) + lugs/ferrules | — | — | power vs signal, crimped |

*(The IRFZ44N pair stays in the drawer as the documented fallback — see the superseded MOSFET
design in the README power section.)*

## 3. Wiring guide — terminal by terminal

Work with the battery disconnected; connect it last.

1. **Battery + → fuse holder (40 A)** — 10 AWG, lug at the battery. Battery − → common
   ground bus (10 AWG).
2. **Fused + → 40 A relay pin 30** (10 AWG) and **fused + → relay-module COM** (18 AWG — the
   logic rail carries ≤ 2–3 A).
3. **Relay module**: `NO` → **+12V_LOG bus** (18 AWG). `VCC` → +12V_LOG bus (yes, its own
   output — the button bootstraps it). `GND` → ground bus. `IN` → **GPIO13** (signal wire);
   set the module's trigger jumper to **LOW**.
4. **START button (momentary)** between fused + and the +12V_LOG bus — in parallel with the
   module's contact. **FORCE ON toggle** likewise, mounted inside the enclosure.
5. **40 A relay coil**: +12V_LOG bus → **e-stop mushroom (NC, top of seatback — signal-gauge
   wires)** → `85`; `86` → ground bus; **1N4007 across 85/86, cathode (ring) on 85**.
6. **40 A relay pin 87 → driver VB+** — 10 AWG, short run inside the nose. Driver VB− →
   ground bus (10 AWG). ⚠️ **Triple-check VB+/VB− polarity before the battery goes in —
   the driver has no reverse protection.**
7. **Sense opto**: LED input through **4.7 kΩ ¼ W** from the **coil node (relay pin 85,
   AFTER the e-stop)** to ground (12 V-rated modules have it onboard; 3.3/5 V modules get ~1–1.5 kΩ
   added); output: collector → **GPIO22**, emitter → GND. No external pull-up needed
   (internal).
8. **Buck**: IN ← +12V_LOG (+ ≥ 470 µF bulk at its input), OUT 5 V → ESP32 5V/VIN, WS2812
   strip (with its 470–1000 µF at the strip head), ground to the bus.
9. **Divider**: 100 k from +12V_LOG to node A0, 15 k from node to ground, 100 nF across the
   15 k; node → **ADS1115 A0**. ADS1115: VCC 3.3 V (from the ESP board), SDA/SCL → GPIO18/19
   (bus 0), ADDR → GND (0x48).
10. **AS5600 L**: SDA/SCL → GPIO18/19 (bus 0, shared with the ADS1115), 3.3 V, GND, 4.7 k
    pull-ups to 3.3 V on both lines. **AS5600 R**: SDA/SCL → GPIO27/14 (bus 1), same recipe.
11. **WS2812**: DIN ← GPIO4 through ~330 Ω; 5 V and GND from the buck.
12. **START sense**: the same physical button also feeds **GPIO16** (to GND, internal
    pull-up) so the firmware sees the press for arming.
13. Separate runs for power (10 AWG) and signal looms; keep the I²C and sense wires away
    from the motor cables.

### Power-up checklist (multimeter, battery just connected)

1. No continuity between +12V rails and ground (before priming).
2. Press START: +12V_LOG present, buck outputs 5.0 V, ESP boots (status LED), release after
   ~1 s — the rail must HOLD.
3. E-stop released: +12V_MOT present at driver VB+; **page shows no MOTOR POWER fault**
   (with `pwr_sense_en=1`).
4. **Press the e-stop**: the page names the `MOTOR POWER` fault (coil sense), re-arm
   required after release; with a healthy relay, +12V_MOT dies too — verify BOTH at
   commissioning (fault shown AND driver VB+ actually dead, multimeter). The routine
   pre-drive test verifies the sense chain; it cannot prove the contact is not welded —
   that case is covered by the software disarm + dynamic brake.
5. **The 30-second brake test**: logic up, e-stop pressed, spin a front wheel by hand — if it
   resists, dynamic braking survives the e-stop on this driver; if it spins free, the e-stop
   coasts (acceptable on the flat only — note the result here: ☐ brakes / ☐ coasts).
6. **Forced-reboot test**: with the kart held on (no capacitor anymore), trigger a reset
   (flash, or watchdog): the kart must power OFF cleanly, and come back with START.

## 4. Decision log (2026-08-02 review)

| Decision | Chosen | Why |
|---|---|---|
| Hold capacitor | **None** | reboot = clean power-off is safe and honest; hidden FORCE ON switch covers bench needs; e-stop timing stays capacitor-free |
| Flyback on 40 A coil | **1N4007 fitted** | the module's contacts break an inductive 150 mA — arcing eats them |
| Reverse-polarity guard | **process, not parts** | polarized connectors + checklist; a 40 A series element wastes watts to guard a one-time mistake |
| Pre-charge on the 40 A contact | **not fitted, watched** | inrush is modest; welding fails ON and is on the watch list; resistor retrofit documented |
| Buck | **verify ≥ 2 A or replace (3 A class)** | firm 5 V ≥ 2 A budget (LEDs + ESP bursts) |
| Divider | **100 k / 15 k confirmed** | matches `hw::VBAT_R_*` and the soldered board; the 27 k row in old tables was a sizing example, now removed |
| Sense position | **on the COIL (85), not the relay output** | e-stop engaged is detected even with a welded contact → software disarm + dynamic brake; cost: welding becomes undetectable and a fail-open relay is only caught by the wheel-stuck net (owner's call over the dual-opto option) |
| E-stop position | **in the 40 A relay's COIL loop** | with no hold capacitor there is nothing to delay it (~10-20 ms drop-out); the mushroom switches 150 mA on thin wires instead of 40 A to the seatback and back; residual welded-contact risk covered by the per-session e-stop test via the sense opto |
