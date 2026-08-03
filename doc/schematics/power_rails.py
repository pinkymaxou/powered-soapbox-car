# power_rails.py — Kart power architecture, TWO-RAIL relay build (current design).
# Single 12 V battery → 40 A fuse → two rails:
#   · LOGIC rail: small opto relay module — primed by the momentary START button (the ESP
#     does not exist yet), held by GPIO13 POWER_HOLD (active low), forceable by a hidden
#     maintenance switch. NO hold capacitor: a reboot drops the rail = clean power-down.
#   · MOTOR rail: 40 A automotive relay (coil fed from the logic rail, 1N4007 flyback),
#     with the E-STOP mushroom breaking the 40 A path mechanically. A sense opto reads the
#     driver-side voltage into GPIO22 (active low = live; 50 ms debounce in firmware).
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
                           fontsize=14).at((7.5, 13.8))
    d += elm.Label().label('named ports: same-name nets are connected · e-stop breaks the 40 A path, the LOGIC stays alive',
                           fontsize=9, color='#555').at((7.5, 13.2))

    # ───────── Battery + master fuse ─────────
    bat = d.add(elm.Ic(pins=[elm.IcPin(name='+', side='right', slot='2/2'),
                             elm.IcPin(name='-', side='right', slot='1/2')],
                       w=2.6, h=2.4, plblsize=11).label('BATTERY 12 V\n(motorcycle)', loc='top', fontsize=9)
                .right().anchor('center').at((-8.5, 9.0)))
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
                .right().anchor('center').at((0.5, 8.6)))
    flag(d, P(mod, 'VCC'), '+12V_LOG', 'left')          # powered by its own output rail:
    d += elm.Label().label('(bootstrap: the button provides\nthe rail until the ESP holds)',
                           fontsize=7, color='#555').at((-3.4, 11.3))
    flag(d, P(mod, 'IN'), 'PWR_HOLD', 'left')
    d += elm.Label().label('GPIO13, active LOW\n(ESP can self power-off)', fontsize=7,
                           color='#555').at((-3.6, 6.9))
    d += elm.Line().left().at(P(mod, 'GND')).length(0.8)
    d += elm.Ground()
    flag(d, P(mod, 'COM'), '+12V_BAT', 'right')          # COM ← battery (fused)
    d += elm.Line().right().at(P(mod, 'NO')).length(1.6)
    d += elm.Dot()
    lognode = d.here
    flag(d, lognode, '+12V_LOG', 'right')

    # Priming button and hidden force switch, both in PARALLEL with the module contact.
    d += elm.Button().right().at((batp[0], 5.6)).label('START\n(momentary, priming)',
                                                       loc='bottom', fontsize=8).length(3.2)
    d += elm.Line().tox(lognode[0])
    d += elm.Line().toy(lognode[1])
    d += elm.Line().down().at(batp).toy(5.6)
    d += elm.Dot()
    d += elm.Line().down().toy(4.0)
    d += elm.Switch().right().label('FORCE ON\n(hidden inside the enclosure,\nbench/flash/diagnostic)',
                                    loc='bottom', fontsize=8).length(3.2)
    d += elm.Line().tox(lognode[0])
    d += elm.Line().toy(lognode[1])
    d += elm.Dot().at(lognode)
    d += elm.Label().label('NO hold capacitor: a reboot (~700 ms) drops the rail →\nclean power-down, re-prime with START',
                           fontsize=8, color=HL).at((3.3, 3.1))

    # ───────── MOTOR rail: 40 A relay + E-STOP in the 40 A path ─────────
    rly = d.add(elm.Ic(pins=[elm.IcPin(name='85', side='left', slot='2/2'),
                             elm.IcPin(name='86', side='left', slot='1/2'),
                             elm.IcPin(name='30', side='top'),
                             elm.IcPin(name='87', side='right', slot='2/2'),
                             elm.IcPin(name='87a', side='right', slot='1/2')],
                       w=3.2, h=2.6, plblsize=10).label('40 A RELAY (automotive)', loc='top', fontsize=9, ofst=(-2.4, 0.6))
                .right().anchor('center').at((11.5, 9.0)))
    d += elm.Line().up().at(P(rly, '30')).length(0.5)
    flag(d, d.here, '+12V_BAT', 'right')
    # Coil from the LOGIC rail: logic dead ⇒ motors dead. 1N4007 kills the coil's kickback
    # so the little module's contacts never break an arcing inductive load.
    d += elm.Line().left().at(P(rly, '85')).length(2.9)
    n85 = d.here
    flag(d, n85, '+12V_LOG', 'left')
    d += elm.Line().left().at(P(rly, '86')).length(1.0)
    n86 = d.here
    d += elm.Dot()
    d += elm.Line().down().at(n86).length(0.7)
    d += elm.Ground()
    # Flyback ACROSS the coil (cathode to +): the module's contacts never break an arc.
    d += elm.Line().left().at(n86).length(0.9)
    dtop = d.add(elm.Diode().up().toy(P(rly, '85')[1]).label('1N4007\nflyback', loc='bottom', fontsize=8, ofst=(-1.3, -0.45)))
    d += elm.Dot()
    d += elm.Label().label('87a: spare (NC)', fontsize=8).at((16.0, 8.0))

    # 87 → E-STOP (mushroom, NC, rated 40 A) → +12V_MOT
    d += elm.Line().right().at(P(rly, '87')).length(1.2)
    d += elm.Switch().right().label('E-STOP\n(mushroom NC, 40 A path,\nMECHANICAL break)',
                                    loc='top', fontsize=9).length(3.0).color(HL)
    d += elm.Dot()
    motnode = d.here
    flag(d, motnode, '+12V_MOT', 'right')

    # ───────── Sense opto: is the motor rail actually live? ─────────
    opto = d.add(elm.Optocoupler(box=True).right().anchor('anode').at((13.2, 4.2)))
    d += elm.Resistor().up().at(opto.anode).label('series R\n(module)', fontsize=8, loc='left', ofst=(-1.3, 0.1)).length(1.5)
    flag(d, d.here, '+12V_MOT', 'up')
    d += elm.Line().down().at(opto.cathode).length(0.8)
    d += elm.Ground()
    d += elm.Line().up().at(opto.collector).length(0.8)
    flag(d, d.here, 'GPIO22', 'up')
    d += elm.Label().label('MOTOR_PWR_SENSE — active LOW = live\ninternal pull-up + 50 ms firmware debounce\nno voltage ⇒ e-stop assumed active',
                           fontsize=8, color='#555').at((18.9, 3.2))
    d += elm.Line().down().at(opto.emitter).length(0.8)
    d += elm.Ground()

    # ───────── Loads of each rail ─────────
    buck = d.add(elm.Ic(pins=[elm.IcPin(name='IN', side='left'), elm.IcPin(name='OUT', side='right')],
                        w=2.8, h=1.8, plblsize=9).label('BUCK 12→5 V — ≥ 2 A cont.', loc='top', fontsize=9)
                 .right().anchor('center').at((0.5, 1.0)))
    flag(d, P(buck, 'IN'), '+12V_LOG', 'left')
    flag(d, P(buck, 'OUT'), '+5V', 'right')
    d += elm.Label().label('+5V: ESP32 (makes its own 3.3 V) + WS2812 strip\nbudget: ~0.6 A LEDs + ~0.7 A ESP bursts + sensors/margin',
                           fontsize=8, color='#555').at((1.6, -0.5))

    drv = d.add(elm.Ic(pins=[elm.IcPin(name='VB+', side='left', slot='2/2'),
                             elm.IcPin(name='LOGIC', side='left', slot='1/2')],
                       w=3.0, h=2.2, plblsize=9).label('MOTOR DRIVER 2× 20 A', loc='top', fontsize=9)
                .right().anchor('center').at((9.0, 0.6)))
    flag(d, P(drv, 'VB+'), '+12V_MOT', 'left')
    flag(d, P(drv, 'LOGIC'), '+12V_LOG', 'left')
    d += elm.Label().label('logic alive with VB+ cut ⇒ dynamic braking\nsurvives the e-stop (bench-test it)',
                           fontsize=8, color='#555').at((10.4, -1.0))

    # Divider on the LOGIC rail (NOT the motor rail: it must not die with the e-stop)
    flag(d, (-8.0, 3.6), '+12V_LOG', 'up')
    d += elm.Resistor().down().at((-7.4, 3.6)).label('100k', fontsize=8).length(1.4)
    d += elm.Dot()
    nv = d.here
    flag(d, nv, 'VBAT→A0', 'right')
    d += elm.Resistor().down().at(nv).label('15k', fontsize=8).length(1.4)
    d += elm.Ground()
    d += elm.Label().label('to ADS1115 A0 (see full_schematic.png)', fontsize=8, color='#555').at((-4.9, 0.35))

print('render OK')
