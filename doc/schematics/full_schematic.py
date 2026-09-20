# full_schematic.py — Kart electrical schematic (differential drive, SINGLE-RELAY power).
# Single 12 V battery; ONE switched rail (+12V_SW) behind the main relay, whose coil runs
# through the main switch and the e-stop mushroom — the switching detail lives in
# power_rails.png. 2 independent REAR motors, 2 AS5600 sensors (one per I2C bus), driven by
# BLUETOOTH GAMEPAD (ESP32 internal radio, no pedal). No ADC and no buttons: the pack is
# always 12 V and nothing measures it, and arming is the gamepad's own START button.
# Generates doc/schematics/full_schematic.png via schemdraw.
#   . .venv-schem/bin/activate && python doc/schematics/full_schematic.py
#
# Convention: net labels (flags) with the SAME NAME are electrically connected
# (as on a multi-sheet schematic). Avoids routing wires across the whole page.
import schemdraw
import schemdraw.elements as elm

schemdraw.config(fontsize=10, lw=1.7)
NET = '#1565c0'

def P(elem, name):
    # absanchors = ABSOLUTE coordinates (anchors = local, untransformed → do not use here)
    return elem.absanchors[name]

def flag(d, xy, name, direction):
    """Named net flag, pointing in 'direction'."""
    t = elm.Tag().at(xy)
    {'left': t.left, 'right': t.right, 'up': t.up, 'down': t.down}[direction]()
    t.label(name, fontsize=9)
    t.color(NET)
    d.add(t)

def header(d, x, y, title, pins):
    n = len(pins)
    h = elm.Header(rows=n, shownumber=False).at((x, y)).label(title, fontsize=9, loc='top')
    d.add(h)
    return h

with schemdraw.Drawing(file='doc/schematics/full_schematic.png', dpi=150, show=False) as d:
    d += elm.Label().label('Differential electric kart — schematic (named ports: same-name nets = connected)',
                           fontsize=15).at((6, 13.0))

    # ───────────────────────── ESP32 (central integrated circuit) ─────────────────────────
    # 2 I2C buses: bus 0 (SDA0/SCL0 = AS5600 L) · bus 1 (SDA1/SCL1 = AS5600 R).
    LEFT = [('18', 'SDA0'), ('19', 'SCL0'), ('27', 'SDA1'), ('14', 'SCL1')]
    RIGHT = [('25', 'PWM_L'), ('26', 'DIR_L'), ('32', 'PWM_R'), ('33', 'DIR_R'),
             ('4', 'WS')]
    pins = []
    for i, (g, _) in enumerate(LEFT):
        pins.append(elm.IcPin(name=g, side='left', slot=f'{len(LEFT)-i}/{len(LEFT)}'))
    for i, (g, _) in enumerate(RIGHT):
        pins.append(elm.IcPin(name=g, side='right', slot=f'{len(RIGHT)-i}/{len(RIGHT)}'))
    pins += [elm.IcPin(name='3V3', side='top'), elm.IcPin(name='5V', side='top'),
             elm.IcPin(name='GND', side='bottom')]
    esp = elm.Ic(pins=pins, label='ESP32\nWROOM', w=4.6, h=9.5, plblsize=12, leadlen=1.1)
    esp.right(); esp.anchor('center'); esp.at((0, 0)); d.add(esp)
    d += elm.Label().label('BT gamepad\n(internal radio)', fontsize=8, color=NET).at((0, -5.6))
    for g, net in LEFT:
        flag(d, P(esp, g), net, 'left')
    for g, net in RIGHT:
        flag(d, P(esp, g), net, 'right')
    d += elm.Line().up().at(P(esp, '3V3')).length(0.9); d += elm.Vdd().label('+3V3')
    d += elm.Line().up().at(P(esp, '5V')).length(0.9); d += elm.Vdd().label('+5V')
    d += elm.Line().down().at(P(esp, 'GND')).length(0.6); d += elm.Ground()

    # ───────────── AS5600 LEFT wheel sensor (bus 0) + pull-ups ─────────────
    hg = header(d, -11.5, 6.5, 'AS5600 rear wheel L (0x36)', ['SDA', 'SCL', '3V3', 'GND'])
    flag(d, P(hg, 'pin1'), 'SDA0', 'left')
    flag(d, P(hg, 'pin2'), 'SCL0', 'left')
    flag(d, P(hg, 'pin3'), '+3V3', 'left')
    d += elm.Ground().at(P(hg, 'pin4'))
    d += elm.Vdd().at((-7.6, 6.9)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-7.6, 6.9)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SDA0', 'down')
    d += elm.Vdd().at((-6.7, 6.9)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-6.7, 6.9)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SCL0', 'down')

    # ───────────── AS5600 RIGHT wheel sensor (bus 1) + pull-ups ─────────────
    hd = header(d, -11.5, 1.4, 'AS5600 rear wheel R (0x36)', ['SDA', 'SCL', '3V3', 'GND'])
    flag(d, P(hd, 'pin1'), 'SDA1', 'left')
    flag(d, P(hd, 'pin2'), 'SCL1', 'left')
    flag(d, P(hd, 'pin3'), '+3V3', 'left')
    d += elm.Ground().at(P(hd, 'pin4'))
    d += elm.Vdd().at((-7.6, 1.8)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-7.6, 1.8)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SDA1', 'down')
    d += elm.Vdd().at((-6.7, 1.8)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-6.7, 1.8)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SCL1', 'down')

    # ───────────────────────── Motor driver + 2 REAR motors ─────────────────────────
    DRV_X, DRV_Y, DRV_W, DRV_H = 10.5, 2.0, 6.0, 5.0
    drv = elm.Ic(pins=[
        elm.IcPin(name='PWM_L', side='left', slot='4/4'), elm.IcPin(name='DIR_L', side='left', slot='3/4'),
        elm.IcPin(name='PWM_R', side='left', slot='2/4'), elm.IcPin(name='DIR_R', side='left', slot='1/4'),
        elm.IcPin(name='M1A', side='right', slot='4/4'), elm.IcPin(name='M1B', side='right', slot='3/4'),
        elm.IcPin(name='M2A', side='right', slot='2/4'), elm.IcPin(name='M2B', side='right', slot='1/4'),
    ], w=DRV_W, h=DRV_H, plblsize=11, leadlen=1.1).label('DRIVER 2× 20 A', loc='top', fontsize=10)
    drv.right(); drv.anchor('center'); drv.at((DRV_X, DRV_Y)); d.add(drv)
    d += elm.Line().up().at((DRV_X, DRV_Y + DRV_H / 2)).length(0.7); d += elm.Vdd().label('+12V_SW (VB+)')
    d += elm.Label().label('board logic on the SAME rail: the e-stop kills both', fontsize=7, color=NET).at((DRV_X, DRV_Y + DRV_H / 2 + 1.6))
    d += elm.Line().down().at((DRV_X, DRV_Y - DRV_H / 2)).length(0.7); d += elm.Ground()
    mL = header(d, 18.0, 3.4, 'REAR MOTOR L', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M1A')).tox(P(mL, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M1B')).tox(P(mL, 'pin2')[0])
    mR = header(d, 18.0, 0.4, 'REAR MOTOR R', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M2A')).tox(P(mR, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M2B')).tox(P(mR, 'pin2')[0])
    d += elm.Label().label('FRONT caster wheel: free (not motorized)', fontsize=8).at((15.5, -1.2))

    # ───────────────────────── WS2812 ─────────────────────────
    hw = header(d, 8.5, -3.6, 'CONN WS2812B', ['DIN', '+5V', 'GND'])
    flag(d, P(hw, 'pin1'), 'WS', 'left')
    flag(d, P(hw, 'pin2'), '+5V', 'right')
    d += elm.Ground().at(P(hw, 'pin3'))

    # ───────────────────────── Power (top) ─────────────────────────
    ba = header(d, -2.5, 11.0, 'BATTERY 12V', ['+', '-'])
    d += elm.Line().right().at(P(ba, 'pin1')).length(1.0)
    d += elm.Fuse().right().label('40 A', fontsize=8, loc='bottom').length(2.0)
    d += elm.Dot(); rail = d.here
    d += elm.Label().label('→ main relay: coil via MAIN SWITCH + E-STOP, contact = +12V_SW (see power_rails.png)',
                           fontsize=8, color=NET).at((rail[0] + 3.4, 12.3))
    flag(d, rail, '+12V_BAT', 'right')
    buck = elm.Ic(pins=[elm.IcPin(name='IN', side='left'), elm.IcPin(name='5V', side='right')],
                  w=2.4, h=1.8, plblsize=9).label('BUCK 12→5V — ≥2 A', loc='top', fontsize=9).right().at((4.2, 10.5)).anchor('IN')
    d.add(buck)
    flag(d, P(buck, 'IN'), '+12V_SW', 'left')
    flag(d, P(buck, '5V'), '+5V', 'right')
    d += elm.Label().label('(+3V3 = ESP32 board regulator)', fontsize=8).at((4.5, 9.0))

    d += elm.Label().label('Power switching detail (main relay + main switch + e-stop + flyback diode): see power_rails.png',
                           fontsize=10).at((0, -8))

print('render OK')
