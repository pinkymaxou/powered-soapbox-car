# power_rails.py — Kart power architecture, TWO-RAIL relay build (current design).
# Single 12 V battery → 40 A fuse → two rails:
#   · LOGIC rail: small opto relay module — primed by the momentary START button (the ESP
#     does not exist yet), held by GPIO13 POWER_HOLD (active low), forceable by a hidden
#     maintenance switch. NO hold capacitor: a reboot drops the rail = clean power-down.
#   · MOTOR rail: 40 A automotive relay; the E-STOP (NC mushroom) breaks the relay's COIL —
#     the relay's contact is what breaks the 40 A, so the mushroom only switches ~150 mA and
#     the power run never detours to the seatback. The sense opto reads the COIL (after the
#     e-stop) into GPIO22: coil dead = e-stop engaged → the firmware disarms and
#     DYNAMIC-BRAKES — which covers even a welded relay contact (driver keeps VB+ + logic).
# Also on sheet: the ESP32 (holds PWR_HOLD, reads GPIO22) and the ADS1115 with the
# 100k/15k divider measuring the battery on the LOGIC rail.
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
    d += elm.Label().label('Two-rail power — 12 V battery, logic relay module + 40 A motor relay',
                           fontsize=14).at((8.5, 15.6))
    d += elm.Label().label('named ports: same-name nets are connected · the E-STOP breaks the 40 A relay COIL; the relay contact breaks the 40 A',
                           fontsize=9, color='#555').at((8.5, 15.0))

    # ───────── Battery + master fuse ─────────
    bat = d.add(elm.Ic(pins=[elm.IcPin(name='+', side='right', slot='2/2'),
                             elm.IcPin(name='-', side='right', slot='1/2')],
                       w=2.6, h=2.4, plblsize=11).label('BATTERY 12 V\n(motorcycle)', loc='top', fontsize=9)
                .right().anchor('center').at((-8.5, 11.0)))
    d += elm.Fuse().right().at(P(bat, '+')).label('40 A', loc='top').length(2.6)
    d += elm.Dot()
    batp = d.here
    flag(d, batp, '+12V_BAT', 'up')
    d += elm.Line().right().at(P(bat, '-')).length(1.2)
    d += elm.Ground().label('common GND', loc='right', fontsize=8)

    # ───────── LOGIC rail: opto relay module + priming button + hidden force switch ─────────
    mod = d.add(elm.Ic(pins=[elm.IcPin(name='VCC', side='left', slot='3/3'),
                             elm.IcPin(name='IN', side='left', slot='2/3'),
                             elm.IcPin(name='GND', side='left', slot='1/3'),
                             elm.IcPin(name='COM', side='right', slot='2/2'),
                             elm.IcPin(name='NO', side='right', slot='1/2')],
                       w=3.4, h=3.0, plblsize=10).label('RELAY MODULE (opto in, low trigger)', loc='top', fontsize=9)
                .right().anchor('center').at((0.5, 10.6)))
    flag(d, P(mod, 'VCC'), '+12V_LOG', 'left')          # powered by its own output rail:
    d += elm.Label().label('(bootstrap: the button provides\nthe rail until the ESP holds)',
                           fontsize=7, color='#555').at((-3.4, 13.3))
    flag(d, P(mod, 'IN'), 'PWR_HOLD', 'left')
    d += elm.Line().left().at(P(mod, 'GND')).length(0.8)
    d += elm.Ground()
    flag(d, P(mod, 'COM'), '+12V_BAT', 'right')          # COM ← battery (fused)
    d += elm.Line().right().at(P(mod, 'NO')).length(1.6)
    d += elm.Dot()
    lognode = d.here
    flag(d, lognode, '+12V_LOG', 'right')

    # Priming button and hidden force switch, both in PARALLEL with the module contact.
    d += elm.Button().right().at((batp[0], 7.6)).label('START\n(momentary, priming)',
                                                       loc='bottom', fontsize=8).length(3.2)
    d += elm.Line().tox(lognode[0])
    d += elm.Line().toy(lognode[1])
    d += elm.Line().down().at(batp).toy(7.6)
    d += elm.Dot()
    d += elm.Line().down().toy(6.0)
    d += elm.Switch().right().label('FORCE ON\n(hidden inside the enclosure,\nbench/flash/diagnostic)',
                                    loc='bottom', fontsize=8).length(3.2)
    d += elm.Line().tox(lognode[0])
    d += elm.Line().toy(lognode[1])
    d += elm.Dot().at(lognode)
    d += elm.Label().label('NO hold capacitor: a reboot (~700 ms) drops the rail →\nclean power-down, re-prime with START',
                           fontsize=8, color=HL).at((3.6, 5.2))

    # ───────── MOTOR rail: 40 A relay, E-STOP in the COIL circuit ─────────
    rly = d.add(elm.Ic(pins=[elm.IcPin(name='85', side='left', slot='2/2'),
                             elm.IcPin(name='86', side='left', slot='1/2'),
                             elm.IcPin(name='30', side='top'),
                             elm.IcPin(name='87', side='right', slot='2/2'),
                             elm.IcPin(name='87a', side='right', slot='1/2')],
                       w=3.2, h=2.6, plblsize=10).label('40 A RELAY (automotive)', loc='top', fontsize=9, ofst=(1.6, 0.9))
                .right().anchor('center').at((13.0, 11.0)))
    d += elm.Line().up().at(P(rly, '30')).length(0.9)
    flag(d, d.here, '+12V_BAT', 'up')
    # E-STOP breaks the COIL: the mushroom switches ~150 mA on thin wires; the relay's
    # CONTACT is what breaks the 40 A. (No hold capacitor anywhere ⇒ drop-out is the relay's
    # own ~10-20 ms.)
    d += elm.Line().left().at(P(rly, '85')).length(2.2)
    n85 = d.here
    d += elm.Dot()
    d += elm.Line().up().at(n85).length(1.5)
    flag(d, d.here, 'COIL_SENSE', 'up')
    d += elm.Switch().left().at(n85).label('E-STOP (mushroom NC,\nin the COIL loop)',
                                           loc='top', fontsize=9).length(3.0).color(HL)
    flag(d, d.here, '+12V_LOG', 'left')
    d += elm.Line().left().at(P(rly, '86')).length(1.0)
    n86 = d.here
    d += elm.Dot()
    d += elm.Line().down().at(n86).length(0.7)
    d += elm.Ground()
    # Flyback ACROSS the coil (cathode to 85/+): the loop stays closed whichever of the
    # e-stop or the module opens, so no contact ever breaks an arcing inductive load.
    d += elm.Line().left().at(n86).length(0.9)
    d += elm.Diode().up().toy(n85[1]).label('1N4007\nflyback', fontsize=8, ofst=(-1.15, -0.5))
    d += elm.Dot()
    d += elm.Label().label('welded contact? the software still stops it: coil sense says\n"e-stop engaged" → disarm + DYNAMIC BRAKE (driver keeps VB+ and logic)',
                           fontsize=8, color=HL).at((13.6, 8.2))
    # 87 → motor rail (the CONTACT is what breaks the 40 A)
    d += elm.Line().right().at(P(rly, '87')).length(1.4)
    flag(d, d.here, '+12V_MOT', 'up')
    d += elm.Label().label('87a: spare (NC)', fontsize=8).at((15.9, 9.5))

    # ───────── Sense opto: is the e-stop engaged? (reads the COIL, after the mushroom) ─────────
    opto = d.add(elm.Optocoupler(box=True).right().anchor('anode').at((20.6, 4.6)))
    d += elm.Resistor().up().at(opto.anode).label('4.7 kΩ ¼ W', fontsize=8, loc='left', ofst=(-1.7, 0.2)).length(1.5)
    flag(d, d.here, 'COIL_SENSE', 'up')
    d += elm.Line().down().at(opto.cathode).length(0.8)
    d += elm.Ground()
    d += elm.Line().up().at(opto.collector).length(0.8)
    flag(d, d.here, 'PWR_SENSE', 'up')
    d += elm.Label().label('E-STOP sense (on the COIL, after the mushroom) — GPIO22\nactive LOW = coil energized = e-stop released\ninternal pull-up + 50 ms firmware debounce\ncoil dead ⇒ e-stop engaged → disarm + dynamic brake',
                           fontsize=8, color='#555').at((26.2, 3.5))
    d += elm.Line().down().at(opto.emitter).length(0.8)
    d += elm.Ground()

    # ───────── Loads of each rail ─────────
    buck = d.add(elm.Ic(pins=[elm.IcPin(name='IN', side='left'), elm.IcPin(name='OUT', side='right')],
                        w=2.8, h=1.8, plblsize=9).label('BUCK 12→5 V — ≥ 2 A cont.', loc='top', fontsize=9)
                 .right().anchor('center').at((-6.8, 2.6)))
    flag(d, P(buck, 'IN'), '+12V_LOG', 'left')
    flag(d, P(buck, 'OUT'), '+5V', 'right')
    d += elm.Label().label('+5V: ESP32 + WS2812 strip — budget:\n~0.6 A LEDs + ~0.7 A ESP bursts + margin',
                           fontsize=8, color='#555').at((-6.4, 0.8))

    drv = d.add(elm.Ic(pins=[elm.IcPin(name='VB+', side='left', slot='2/2'),
                             elm.IcPin(name='LOGIC', side='left', slot='1/2')],
                       w=3.0, h=2.2, plblsize=9).label('MOTOR DRIVER 2× 20 A', loc='top', fontsize=9)
                .right().anchor('center').at((21.0, 10.6)))
    flag(d, P(drv, 'VB+'), '+12V_MOT', 'left')
    flag(d, P(drv, 'LOGIC'), '+12V_LOG', 'left')
    d += elm.Label().label('logic alive with VB+ cut ⇒ dynamic braking\nsurvives the e-stop (bench-test it)',
                           fontsize=8, color='#555').at((22.4, 8.9))

    # ───────── ESP32: holds the rail, reads the sense ─────────
    esp = d.add(elm.Ic(pins=[elm.IcPin(name='5V', side='left', slot='2/2'),
                             elm.IcPin(name='GND', side='left', slot='1/2'),
                             elm.IcPin(name='3V3', side='right', slot='3/3'),
                             elm.IcPin(name='13', side='right', slot='2/3'),
                             elm.IcPin(name='22', side='right', slot='1/3')],
                       w=3.2, h=3.0, plblsize=10).label('ESP32-WROOM', loc='top', fontsize=9)
                .right().anchor('center').at((0.0, 1.8)))
    flag(d, P(esp, '5V'), '+5V', 'left')
    d += elm.Line().left().at(P(esp, 'GND')).length(0.8)
    d += elm.Ground()
    flag(d, P(esp, '3V3'), '+3V3', 'right')
    flag(d, P(esp, '13'), 'PWR_HOLD', 'right')
    flag(d, P(esp, '22'), 'PWR_SENSE', 'right')
    d += elm.Label().label('GPIO13 low = hold the logic rail\nGPIO22 = e-stop sense on the 40 A coil (pull-up on)',
                           fontsize=8, color='#555').at((0.4, -0.8))

    # ───────── ADS1115 + divider: battery voltage on the LOGIC rail ─────────
    flag(d, (8.0, 4.6), '+12V_LOG', 'up')
    d += elm.Line().right().at((8.0, 4.6)).length(0.6)
    d += elm.Resistor().down().label('100k', fontsize=8).length(1.6)
    d += elm.Dot()
    nv = d.here
    d += elm.Resistor().down().at(nv).label('15k', fontsize=8).length(1.6)
    d += elm.Ground()
    ads = d.add(elm.Ic(pins=[elm.IcPin(name='A0', side='left', slot='3/3'),
                             elm.IcPin(name='VCC', side='left', slot='2/3'),
                             elm.IcPin(name='GND', side='left', slot='1/3'),
                             elm.IcPin(name='SDA', side='right', slot='2/2'),
                             elm.IcPin(name='SCL', side='right', slot='1/2')],
                       w=3.0, h=2.8, plblsize=10).label('ADS1115 (0x48)', loc='top', fontsize=9)
                .right().anchor('center').at((13.4, 2.2)))
    d += elm.Line().left().at(P(ads, 'A0')).tox(nv[0])   # divider node straight into A0
    d += elm.Line().toy(nv[1])
    d += elm.Dot().at(nv)
    flag(d, P(ads, 'VCC'), '+3V3', 'left')
    d += elm.Line().left().at(P(ads, 'GND')).length(0.8)
    d += elm.Ground()
    d += elm.Label().label('SDA/SCL → I²C bus 0 (GPIO18/19,\nshared with AS5600 L — see full_schematic.png)',
                           fontsize=8, color='#555').at((17.8, 1.6))
    d += elm.Label().label('divider on the LOGIC rail: with the e-stop pressed the battery\nstays measurable (the MOTOR rail is the one that dies)',
                           fontsize=8, color='#555').at((9.6, -1.2))

print('render OK')
