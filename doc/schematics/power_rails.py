# power_rails.py — Kart power architecture, SINGLE-RELAY build (current design).
# Single 12 V battery → 40 A fuse → ONE main relay that carries the WHOLE kart:
#   · COIL circuit: +12V_BAT → MAIN SWITCH → E-STOP (NC mushroom) → coil 85; 86 → GND.
#     Opening either one drops the coil, the contact opens, and everything dies together —
#     motors AND brain. There is no second rail and no software involvement whatsoever:
#     the firmware cannot hold its own power and cannot cut it (GPIO13/GPIO22 are free now).
#   · CONTACT (30 → 87): the 40 A path feeding the buck (5 V logic) and the driver's VB+.
#     Because EVERYTHING is behind that contact, a reversed battery reaches nothing at all.
#   · 1N4007 across the coil (cathode to 85/+): flyback for the switch contacts — and, as a
#     side effect, REVERSE-POLARITY protection for the whole kart: wire the battery backwards
#     and the diode conducts, the fuse blows, and the relay never closes, so nothing
#     downstream (the driver above all, which has no protection of its own) ever sees it.
# Nothing measures the battery any more: no divider, no ADS1115, no LVC. Always 12 V.
#   . .venv-schem/bin/activate && python doc/schematics/power_rails.py
import schemdraw
import schemdraw.elements as elm

schemdraw.config(fontsize=10, lw=1.8)
NET = '#1565c0'
HL = '#b71c1c'


def P(elem, name):
    return elem.absanchors[name]


def flag(d, xy, name, direction, color=NET):
    t = elm.Tag().at(xy)
    {'left': t.left, 'right': t.right, 'up': t.up, 'down': t.down}[direction]()
    t.label(name, fontsize=9)
    t.color(color)
    d.add(t)


with schemdraw.Drawing(file='doc/schematics/power_rails.png', dpi=150, show=False) as d:
    d += elm.Label().label('Single-relay power — 12 V battery, one main relay for the whole kart',
                           fontsize=14).at((7.0, 15.4))
    d += elm.Label().label('named ports: same-name nets are connected · MAIN SWITCH and E-STOP are in series in the relay COIL; the contact carries everything',
                           fontsize=9, color='#555').at((7.0, 14.8))

    # ───────── Battery + master fuse ─────────
    bat = d.add(elm.Ic(pins=[elm.IcPin(name='+', side='right', slot='2/2'),
                             elm.IcPin(name='-', side='right', slot='1/2')],
                       w=2.6, h=2.4, plblsize=11).label('BATTERY 12 V\n(motorcycle)', loc='top', fontsize=9)
                .right().anchor('center').at((-9.0, 11.0)))
    d += elm.Fuse().right().at(P(bat, '+')).label('40 A', loc='top').length(2.4)
    d += elm.Dot()
    batp = d.here
    flag(d, batp, '+12V_BAT', 'up')
    d += elm.Line().right().at(P(bat, '-')).length(1.2)
    d += elm.Ground().label('common GND', loc='right', fontsize=8)

    # ───────── The ONE relay: coil switched by MAIN + E-STOP, contact carries the kart ─────────
    rly = d.add(elm.Ic(pins=[elm.IcPin(name='85', side='left', slot='2/2'),
                             elm.IcPin(name='86', side='left', slot='1/2'),
                             elm.IcPin(name='30', side='top'),
                             elm.IcPin(name='87', side='right', slot='2/2'),
                             elm.IcPin(name='87a', side='right', slot='1/2')],
                       w=3.2, h=2.6, plblsize=10).label('MAIN RELAY 40 A (automotive)',
                                                        loc='top', fontsize=9, ofst=(1.6, 0.9))
                .right().anchor('center').at((11.0, 11.0)))
    d += elm.Line().up().at(P(rly, '30')).length(0.9)
    flag(d, d.here, '+12V_BAT', 'up')
    d += elm.Line().right().at(P(rly, '87')).length(1.4)
    flag(d, d.here, '+12V_SW', 'up')
    d += elm.Label().label('87a: spare (NC)', fontsize=8).at((13.9, 9.5))

    # Coil loop: 85 ← e-stop ← main switch ← battery+. Both switches see the coil's ~150 mA
    # on thin wire; the CONTACT is what carries the 40 A.
    d += elm.Line().left().at(P(rly, '85')).length(1.4)
    n85 = d.here
    d += elm.Dot()
    d += elm.Switch().left().at(n85).label('E-STOP (mushroom NC,\nin the COIL loop)',
                                           loc='top', fontsize=9).length(3.0).color(HL)
    d += elm.Switch().left().label('MAIN SWITCH\n(key/toggle, on the dash)',
                                   loc='bottom', fontsize=9).length(3.0)
    flag(d, d.here, '+12V_BAT', 'left')
    d += elm.Line().left().at(P(rly, '86')).length(1.0)
    n86 = d.here
    d += elm.Dot()
    d += elm.Line().down().at(n86).length(0.7)
    d += elm.Ground()
    # Flyback across the coil, cathode to 85 (+).
    d += elm.Line().left().at(n86).length(0.9)
    d += elm.Diode().up().toy(n85[1]).label('1N4007\nflyback', fontsize=8, ofst=(-1.15, -0.5))
    d += elm.Dot()
    d += elm.Label().label('either switch opens ⇒ coil drops ⇒ the WHOLE kart goes dark in ~10-20 ms\n'
                           '(motors AND ESP32 — the firmware never sees it, and nothing brakes:\n'
                           'with no power the kart COASTS — 15 m from full speed on the flat)',
                           fontsize=8, color=HL).at((11.6, 7.9))
    d += elm.Label().label('REVERSE POLARITY: the diode conducts and shorts the coil, so the relay\n'
                           'NEVER CLOSES — and since everything hangs off that contact,\n'
                           'no current reaches the system at all (replace the diode, check the fuse)',
                           fontsize=8, color=HL).at((3.2, 13.6))

    # ───────── Loads: everything hangs off the contact ─────────
    buck = d.add(elm.Ic(pins=[elm.IcPin(name='IN', side='left'), elm.IcPin(name='OUT', side='right')],
                        w=2.8, h=1.8, plblsize=9).label('BUCK 12→5 V — ≥ 2 A cont.', loc='top', fontsize=9)
                 .right().anchor('center').at((-7.0, 5.0)))
    flag(d, P(buck, 'IN'), '+12V_SW', 'left')
    flag(d, P(buck, 'OUT'), '+5V', 'right')
    d += elm.Label().label('+5V: ESP32 + WS2812 strip — budget:\n~0.6 A LEDs + ~0.7 A ESP bursts + margin',
                           fontsize=8, color='#555').at((-6.6, 3.2))

    drv = d.add(elm.Ic(pins=[elm.IcPin(name='VB+', side='left', slot='2/2'),
                             elm.IcPin(name='LOGIC', side='left', slot='1/2')],
                       w=3.0, h=2.2, plblsize=9).label('MOTOR DRIVER 2× 20 A', loc='top', fontsize=9)
                .right().anchor('center').at((19.0, 11.0)))
    flag(d, P(drv, 'VB+'), '+12V_SW', 'left')
    flag(d, P(drv, 'LOGIC'), '+12V_SW', 'left')
    d += elm.Label().label('driver logic and VB+ now die together:\nno dynamic braking after the cut (see above)',
                           fontsize=8, color='#555').at((20.4, 9.2))

    # ───────── ESP32: a plain load now ─────────
    esp = d.add(elm.Ic(pins=[elm.IcPin(name='5V', side='left', slot='2/2'),
                             elm.IcPin(name='GND', side='left', slot='1/2'),
                             elm.IcPin(name='3V3', side='right')],
                       w=3.2, h=2.4, plblsize=10).label('ESP32-WROOM', loc='top', fontsize=9)
                .right().anchor('center').at((0.0, 1.4)))
    flag(d, P(esp, '5V'), '+5V', 'left')
    d += elm.Line().left().at(P(esp, 'GND')).length(0.8)
    d += elm.Ground()
    flag(d, P(esp, '3V3'), '+3V3', 'right')
    d += elm.Label().label('no power-latch pin, no e-stop sense, no arming button:\n'
                           'GPIO13, 16 and 22 are FREE and the board has NO input.\n'
                           'Power is the main switch; arming is a gamepad button.',
                           fontsize=8, color='#555').at((1.0, -1.2))
    d += elm.Label().label('+3V3: AS5600 ×2 (I²C buses 0 and 1) — see full_schematic.png',
                           fontsize=8, color='#555').at((7.6, 2.2))

print('render OK')
