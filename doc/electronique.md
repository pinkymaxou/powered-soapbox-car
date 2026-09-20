# Electronics — design, BOM and wiring guide

The electronics chapter of the kart, in the same spirit as [`reducteur.md`](reducteur.md) for
the drivetrain: every choice with its **why**, the verified parts list, and the
terminal-by-terminal wiring guide. The firmware side of every signal named here is described
in [`../firmware/README.md`](../firmware/README.md).

**Design inputs (fixed):** single **12 V** motorcycle battery — always 12 V, never measured ·
**5 V rail ≥ 2 A** (it powers everything: ESP32, WS2812 strip, sensors) · **ONE main relay
carries the whole kart**, its coil in series with the **main switch** and the **e-stop
mushroom** · **1N4007 across that coil** (flyback — and, because nothing is powered until the
relay closes, reverse-polarity protection for the whole vehicle) · ESP32 and 2× AS5600
confirmed · **no ADC, no voltage divider, no power latch, no buttons**: the ESP32 has no GPIO
input left at all.

![Single-relay power](schematics/power_rails.png)

> Regenerate: `. .venv-schem/bin/activate && python doc/schematics/power_rails.py`
> (environment: [`schematics/requirements.txt`](schematics/requirements.txt)). The whole-kart
> schematic is [`schematics/full_schematic.png`](schematics/full_schematic.png).

## 1. Architecture — one relay, one rail

| | |
|---|---|
| **Coil circuit** (~150 mA, thin wire) | fused battery + → **MAIN SWITCH** → **E-STOP (NC mushroom)** → pin **85**; **86** → ground. **1N4007 across 85/86**, cathode on 85. |
| **Contact** (30 → 87, the 40 A path) | feeds **+12V_SW**: the buck (→ 5 V: ESP32, WS2812), the motor driver's **VB+** *and* its logic board. |
| **Dies when** | the main switch is turned off, the mushroom is pressed, the fuse blows, or the battery is disconnected. There is no fourth case — **the firmware has no say in it at all**. |

This replaces the old two-rail design (logic relay module held by `POWER_HOLD` + separate
40 A motor relay + coil-sense opto on GPIO22). What was bought for that complexity, and what
it costs to drop it, is spelled out below — the trade is real, and it is deliberate.

### What the e-stop now does — and what it does not

Pressing the mushroom opens the coil loop, the contact opens in ~10–20 ms, and **the whole
kart goes dark: motors and brain together**. Consequences, stated plainly:

- **Nothing brakes.** The driver's MOSFETs open, the motors freewheel. Simulated and asserted
  in [`scenarios.hpp`](../firmware/test_host/sim/scenarios.hpp) (`arret_urgence_plat`): hit at
  full speed on the flat, the kart **coasts about 15 m** on rolling resistance alone before
  stopping. On a slope it does not stop at all — `coupure_pente8` reaches 4.4 m/s eight
  seconds after the cut, `coupure_pente16` 12 m/s. **The e-stop is the last resort, not the
  brake.** The brake is releasing the stick (dynamic or PID braking, both need power).
- **Nothing is reported.** No `MOTOR POWER` fault, no event-log record, no page notification —
  the firmware that would write them is off. The web page simply loses its connection.
- **Nothing is remembered.** Power returns on the main switch, the ESP32 boots, and the kart
  comes up **disarmed**, as it does after any boot: a deliberate hold on the gamepad's START
  is required. The
  old "re-arm after the e-stop" rule is now automatic rather than enforced in software.
- **A welded contact is invisible.** The old coil sense caught that case in software; nothing
  does now. It is on the watch list (see the decision log), and the counter-measure is the
  commissioning test below: press the mushroom, verify with a multimeter that VB+ actually dies.

> If coasting after a power cut is ever judged unacceptable on a slope, the hardware fix is a
> **normally-closed relay shorting the motor phases** when power goes away — dynamic braking
> that survives the cut. Not fitted; documented because the simulation quantifies exactly what
> it would buy.

### Start / stop

- **Start**: turn the **main switch** on (key or toggle, on the dash, e-stop released). The
  relay closes, the buck comes up, the ESP32 boots in ~700 ms and starts **disarmed**.
- **Arm**: hold the gamepad's **START/Options** button for 1 s with the stick centered. There
  is **no button on the kart** any more — the arming button on GPIO16 is gone with the rest of
  the GPIO inputs. Arming already demanded a connected, calibrated gamepad, so the button only
  ever duplicated a control the driver was holding anyway; removing it also means nothing on
  the vehicle can be pressed by a bystander.
- **Stop**: main switch off, or the mushroom. Both are hardware, both are absolute.
- **Reboot is no longer a power-down.** With the old latch, a watchdog reset dropped the rail
  and turned the kart off; now the relay is held by a physical switch, so the ESP32 simply
  reboots and comes back disarmed. That is strictly better: a reset while driving costs ~1 s
  of dead time (motors unpowered, gearbox not back-drivable) instead of a total shutdown.
- **No hidden FORCE ON switch**, no priming button, no hold capacitor: the main switch *is*
  the bench switch, and USB power alone runs the ESP32 for flashing with the relay open.

### The diode, and the reverse-polarity protection nobody paid for

**1N4007 across the coil, cathode to 85 (+).** Its job is flyback: the coil is an inductor and
whichever contact opens it — the main switch or the mushroom — would otherwise break a few
hundred volts of kickback and erode itself. Both switches are ordinary hardware, so this is
not optional.

**The side effect protects the entire kart, and it follows from the single relay.** Because
*everything* now sits behind the contact, the only question a reversed battery has to answer
is "does the relay close?". It does not: with the polarity reversed the diode is
forward-biased across the coil, so the coil is shorted and never pulls in. The contact stays
open, **no current reaches the system at all** — not the motor driver (which has no reverse
protection of its own and dies instantly from reversed VB+), not the buck, not the ESP32, not
the sensors. The two-rail design could not have claimed that: its logic rail was fed through a
separate module, on its own path, with its own exposure.

The old design answered this risk with "process, not parts" — polarized connectors and a
checklist. The process still applies; now the topology backs it up, for free.

⚠️ Two things the diode does not do:
- It does not survive the event politely. Shorted across a reversed 12 V through thin coil
  wiring, the 1N4007 (1 A continuous) and the 40 A fuse race each other — **expect to replace
  the diode, and check the fuse**, before concluding that anything is fixed. A relay that no
  longer clicks after a polarity mistake is a dead diode, not a dead relay.
- It does not cover a reversal made **downstream of the contact** — VB+/VB− swapped by hand at
  the driver's own terminals, which is on the other side of the relay. The polarity check at
  step 6 of the wiring guide is still the only thing standing between that mistake and a dead
  driver.

### 5 V rail — the ≥ 2 A budget

| Consumer | Worst case | Note |
|---|---:|---|
| WS2812 ×10 | ~0.60 A | 60 mA/LED full white; status colors at brightness 64 draw far less |
| ESP32-WROOM (via its 3.3 V LDO) | ~0.70 A | Wi-Fi TX bursts; sustained is ~0.24 A |
| AS5600 ×2 | ~0.02 A | on 3.3 V, through the ESP board's regulator |
| Motor-driver logic inputs | ~0.02 A | PWM/DIR are 3.3 V signals; board logic is on +12V_SW |
| Margin / future (buzzer, lights) | ~0.6 A | |
| **Total** | **≈ 1.9 A** | **buck must sustain 2 A continuous at 11–14.8 V in** |

The on-hand buck is a 20 V-rated module reused at 12 V input — **verify its continuous
rating** (label/heatsink): if it is not clearly ≥ 2 A (3 A class recommended), replace it.
At the WS2812 strip head: the classic **470–1000 µF electrolytic across 5 V/GND** and
**~330 Ω in series with DIN** (first-LED protection), data wire kept short.

### The battery is not measured

One 12 V motorcycle pack, which is also the motors' nominal voltage. Nothing scales it down,
nothing reads it: **no ADS1115, no 100 k/15 k divider, no low-voltage cutoff, no 12/24 V
detection, no automatic PWM cap**. The only power ceiling left in the firmware is the manual
`duty_cap` setting on the config page.

What this gives up, honestly: the kart will no longer warn you about a flat pack, refuse to
drive on one, or cut its own power after 30 s of low voltage. A worn battery now shows up as a
kart that simply **feels slower** — simulated in `batterie_usee` (11.2 V, 0.12 Ω: it drives, at
reduced speed, with no fault of any kind). Deep-discharge protection is the pack's own
business. Check the charge on the battery's indicator, off the kart, and charge it on a
schedule rather than on a warning.

### Protections

| Risk | Measure |
|---|---|
| Short / overload | **40 A blade fuse** in a holder, as close to the battery + as possible — everything downstream is protected, relay included |
| Reversed battery connection | **1N4007 across the coil** (see above): the fuse blows and the relay never closes. Plus polarized connectors + color discipline (red/+, black/−) |
| Coil kickback eroding the switches | that same **1N4007 across 85/86** |
| ESP32 brownout during motor surges | bulk electrolytic (≥ 470 µF) at the buck input; keep the buck's input tap off the motor cables' IR drop |
| No braking after a power cut | **behavioral**: the e-stop is a last resort; the brake is the stick. Hardware option documented above (NC relay across the phases) |

## 2. BOM (electronics)

| # | Part | Spec / rating | Qty | Role |
|--:|---|---|--:|---|
| 1 | Motorcycle battery | 12 V lead-acid, ≥ 40 A peak | 1 | single pack, **in the NOSE** — strapped in a retaining tray above the front caster (counterweight) |
| 2 | Blade fuse + holder | **40 A** | 1 | master protection at battery + |
| 3 | Automotive relay | 12 V coil, **40 A** on 87 (NO), SPDT | 1 | **the** main relay: whole kart |
| 4 | Diode 1N4007 | 1 A / 1000 V | 1 | flyback across the coil + reverse-polarity guard |
| 5 | E-stop mushroom | NC, latching (coil current only: ≥ 1 A) | 1 | in the coil loop, top of seatback — thin wires |
| 6 | Main switch | toggle or key, ≥ 1 A, panel mount | 1 | in the coil loop, on the dash — the on/off of the kart |
| 7 | Buck converter | 12 V in → **5 V ≥ 2 A cont.** (3 A class) | 1 | 5 V rail |
| 8 | ESP32-WROOM board | dual-core, 4 MB | 1 | controller |
| 9 | Motor driver | dual channel, 20 A/ch, 6–30 V, PWM+DIR | 1 | both rear (driven) motors |
| 10 | AS5600 breakout + diametric magnet | 12-bit angle, I²C 0x36 | 2 | one per wheel, one per bus |
| 11 | Resistors 4.7 k | ¼ W | 4 | I²C pull-ups (2 per bus) |
| 12 | Resistor ~330 Ω | ¼ W | 1 | WS2812 DIN series |
| 13 | Capacitors: ≥ 470 µF (buck in), 470–1000 µF (LED strip) | 16 V+ | 2 | bulk + decoupling |
| 14 | WS2812B strip | ~10 LEDs, 5 V | 1 | status display (GPIO4) |
| 15 | Enclosure ~150×100×70, clear lid | ≈ IP65 + cable glands | 1 | ESP32 + perfboard |
| 16 | Wire: 10 AWG (power), 18–22 AWG (signal) + lugs/ferrules | — | — | power vs signal, crimped |

*Dropped from the previous build: the opto relay module, the second relay, the coil-sense
optocoupler, the ADS1115, the 100 k/15 k divider pair, the 100 nF at A0, the hidden FORCE ON
toggle, the priming/arming momentary button — and with it the last GPIO input on the board.*

## 3. Wiring guide — terminal by terminal

Work with the battery disconnected; connect it last.

1. **Battery + → fuse holder (40 A) AT the battery**, in the **nose tray above the front
   caster** — the battery, the relay, the buck and the driver all live in that bay, so these
   runs are short. The long pair is the **motor wiring**: 10 AWG **nose→rear** along a frame
   rail to the two driven wheels (≈ 1.1 m each way: ~7 mΩ round trip, ~0.3 V at 40 A — fine).
   Battery − → common ground bus (10 AWG). Keep signal looms on the other rail, away from
   that run.
2. **Fused + → relay pin 30** (10 AWG).
3. **Coil loop** (18–22 AWG): fused + → **main switch** (dash) → **e-stop mushroom (NC, top of
   seatback)** → **85**; **86** → ground bus. **1N4007 across 85/86, cathode (ring) on 85.**
   Order matters only for reach — either switch cuts the same loop.
4. **Relay pin 87 → +12V_SW bus** (10 AWG, short run inside the nose). **87a stays spare.**
5. **+12V_SW → driver VB+** (10 AWG) and **→ driver logic supply** (18 AWG). Driver VB− →
   ground bus (10 AWG). The driver's two motor outputs are what run nose→rear (step 1). ⚠️ **Triple-check VB+/VB− polarity before the battery goes in — the
   driver has no reverse protection, and it sits on the far side of the relay contact, so the
   coil diode cannot save it from a swap made here.**
6. **Buck**: IN ← +12V_SW (+ ≥ 470 µF bulk at its input), OUT 5 V → ESP32 5V/VIN, WS2812
   strip (with its 470–1000 µF at the strip head), ground to the bus.
7. **AS5600 L**: SDA/SCL → GPIO18/19 (bus 0), 3.3 V, GND, 4.7 k pull-ups to 3.3 V on both
   lines. **AS5600 R**: SDA/SCL → GPIO27/14 (bus 1), same recipe.
8. **WS2812**: DIN ← GPIO4 through ~330 Ω; 5 V and GND from the buck.
9. Separate runs for power (10 AWG) and signal looms; keep the I²C wires away from the
   motor cables. **Nothing else connects to the ESP32**: no buttons, no analog inputs, no
   sense lines — the two I²C buses, the four motor-control pins and the LED data line are the
   whole harness on the logic side.

### Power-up checklist (multimeter, battery just connected)

1. No continuity between +12V_SW and ground, **with the main switch off** (before anything).
2. **Main switch on**: the relay clicks, +12V_SW present, buck outputs 5.0 V, ESP32 boots
   (status LED, then the Wi-Fi access point). The page shows the kart **disarmed**.
3. **Press the e-stop**: the relay drops, **everything** goes dark — verify with the
   multimeter that **driver VB+ is actually at 0 V** (this is the only test that catches a
   welded contact; nothing in software can). The web page loses its connection, which is the
   expected symptom, not a fault.
4. **Release the e-stop**: the kart powers back up and comes back **disarmed** — it must NOT
   resume driving on its own. Re-arm with the gamepad's START.
5. **Coast test (do it once, know the number)**: on a flat open surface, at moderate speed,
   press the mushroom. The kart **keeps rolling** — measure roughly how far. The simulation
   says ~15 m from full speed; feel it at half speed so the number means something when it
   matters. **Never test this on a slope.**
6. **Reboot test**: trigger a reset while powered (flash, or watchdog). The kart must stay
   powered, reboot in ~1 s and come back **disarmed** (this is a change from the old latch,
   where a reset powered the kart off).

## 4. Decision log

### 2026-09-20 review — the simplification

| Decision | Chosen | Why |
|---|---|---|
| Power architecture | **ONE relay for the whole kart** | the two-rail split existed so the brain could survive the e-stop and report it; that reporting bought a fault message, not a stop — and it cost a second relay, an opto, a sense pin and a software path. One relay, one rail, one failure mode |
| E-stop position | **in the coil loop, with the main switch** | the mushroom switches ~150 mA on thin wires; the contact breaks the 40 A. Unchanged from the previous build, and still the reason the power run stays in the nose |
| Coil-sense opto (GPIO22) | **removed** | it only mattered while the logic survived the cut. With the ESP32 on the same relay there is nothing left to sense: no `MOTOR POWER` fault, no debounce, no bench jumper |
| Power latch (GPIO13) | **removed** | a firmware that can cut its own supply needs a priming button, a bootstrap rail, a FORCE ON switch and a "reboot = power-down" rule. A main switch does the same job with no failure modes and no code |
| Idle auto power-off | **removed with the latch** | it needed `POWER_HOLD`. A forgotten kart is now switched off by hand — the same hand that switched it on |
| Battery measurement (ADS1115 + divider) | **removed** | one 12 V pack, which is the motors' nominal voltage: nothing to cap automatically. Cost, stated: no LVC, no low-battery warning, no gauge — a flat pack is felt, not reported. The pack's own protection remains |
| Reverse-polarity protection | **the coil diode + the single relay, by design now** | the 1N4007 was already required for flyback. Reversed, it shorts the coil so the relay never closes — and since the single relay feeds *everything*, no current reaches any part of the system. The two-rail build could not have made that claim. Free; budget a replacement diode |
| Arming button (GPIO16) | **removed** | arming already required a connected, calibrated gamepad holding its own START button, so the panel button duplicated a control the driver had in hand. Removing it takes the last GPIO input off the board and leaves nothing on the vehicle a bystander can press |
| Welded relay contact | **not detected, tested at commissioning** | the coil sense used to catch it in software. Now step 3 of the checklist (VB+ dead with the mushroom pressed) is the only test — deliberately accepted with the single-relay trade |
| Braking after the e-stop | **none — documented and measured** | with no power there is no electric brake of any kind. `arret_urgence_plat` puts a number on it (~15 m on the flat) so the behaviour is a known quantity rather than a surprise. NC relay across the phases stays the documented fix if ever wanted |

### 2026-08-02 review (previous build — kept for the record)

| Decision | Chosen then | Note |
|---|---|---|
| Hold capacitor | none | moot: there is no latch to hold up any more |
| Flyback on the 40 A coil | 1N4007 fitted | **kept**, and now doing double duty (see above) |
| Reverse-polarity guard | process, not parts | **superseded**: the coil diode backs the process up |
| Pre-charge on the contact | not fitted, watched | **still true** — the relay closes with the motors commanded off (boot sequence), so the make-current is the driver's capacitor inrush alone |
| Divider 100 k / 15 k | confirmed | **removed** with the ADC |
| Software bypass of the sense | removed (`pwr_sense_en` deleted) | moot: the sense itself is gone |
| Sense position | on the coil, not the relay output | moot: no sense |
