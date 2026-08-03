# full_schematic.py — Kart electrical schematic (differential drive, TWO-RAIL power).
# Single 12 V battery; +12V_LOG (logic rail, held by the relay module) and +12V_MOT
# (motor rail, 40 A relay + e-stop) — the switching detail lives in power_rails.png.
# 2 independent FRONT motors, 2 AS5600 sensors (one per I2C bus), external ADS1115 ADC
# (Vbat), driven by BLUETOOTH GAMEPAD (ESP32 internal radio, no pedal).
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
    # 2 I2C buses: bus 0 (SDA0/SCL0 = AS5600 L + ADS1115) · bus 1 (SDA1/SCL1 = AS5600 R).
    LEFT = [('18', 'SDA0'), ('19', 'SCL0'), ('27', 'SDA1'), ('14', 'SCL1'), ('16', 'START')]
    RIGHT = [('25', 'PWM_L'), ('26', 'DIR_L'), ('32', 'PWM_R'), ('33', 'DIR_R'),
             ('4', 'WS'), ('22', 'PWR_SENSE'), ('13', 'PWR_HOLD')]
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

    # ───────────── External ADS1115 ADC (bus 0) + battery voltage on A0 ─────────────
    ha = header(d, -11.5, 9.0, 'ADS1115 (0x48)', ['A0', 'SDA', 'SCL', '3V3', 'GND'])
    flag(d, P(ha, 'pin1'), 'VBAT', 'left')     # A0 = divider midpoint (VBAT net)
    flag(d, P(ha, 'pin2'), 'SDA0', 'left')
    flag(d, P(ha, 'pin3'), 'SCL0', 'left')
    flag(d, P(ha, 'pin4'), '+3V3', 'left')
    d += elm.Ground().at(P(ha, 'pin5'))
    d += elm.Label().label('A1/A2: FUTURE joystick', fontsize=7).at((-10.2, 6.3))
    # 100k/15k divider: +20V → VBAT → GND (VBAT goes to A0)
    flag(d, (-7.6, 10.4), '+12V_LOG', 'up'); vtop = (-7.0, 10.4)
    d += elm.Resistor().down().at(vtop).label('100k', fontsize=8).length(1.4)
    d += elm.Dot(); nv = d.here
    flag(d, nv, 'VBAT', 'right')
    d += elm.Resistor().down().at(nv).label('15k', fontsize=8).length(1.4)
    d += elm.Ground()

    # ───────────── AS5600 LEFT wheel sensor (bus 0) + pull-ups ─────────────
    hg = header(d, -11.5, 4.0, 'AS5600 wheel L (0x36)', ['SDA', 'SCL', '3V3', 'GND'])
    flag(d, P(hg, 'pin1'), 'SDA0', 'left')
    flag(d, P(hg, 'pin2'), 'SCL0', 'left')
    flag(d, P(hg, 'pin3'), '+3V3', 'left')
    d += elm.Ground().at(P(hg, 'pin4'))
    d += elm.Vdd().at((-7.6, 4.4)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-7.6, 4.4)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SDA0', 'down')
    d += elm.Vdd().at((-6.7, 4.4)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-6.7, 4.4)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SCL0', 'down')

    # ───────────── AS5600 RIGHT wheel sensor (bus 1) + pull-ups ─────────────
    hd = header(d, -11.5, -0.6, 'AS5600 wheel R (0x36)', ['SDA', 'SCL', '3V3', 'GND'])
    flag(d, P(hd, 'pin1'), 'SDA1', 'left')
    flag(d, P(hd, 'pin2'), 'SCL1', 'left')
    flag(d, P(hd, 'pin3'), '+3V3', 'left')
    d += elm.Ground().at(P(hd, 'pin4'))
    d += elm.Vdd().at((-7.6, -0.2)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-7.6, -0.2)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SDA1', 'down')
    d += elm.Vdd().at((-6.7, -0.2)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-6.7, -0.2)).length(1.2).label('4k7', fontsize=8)
    flag(d, d.here, 'SCL1', 'down')

    # ───────────────────────── START button (arming) ─────────────────────────
    hb = header(d, -11.5, -5.0, 'CONN START', ['S', 'GND'])
    flag(d, P(hb, 'pin1'), 'START', 'left'); d += elm.Ground().at(P(hb, 'pin2'))
    d += elm.Label().label("(also armed by the gamepad START button)", fontsize=7).at((-9.2, -6.4))

    # ───────────────────────── Motor driver + 2 FRONT motors ─────────────────────────
    DRV_X, DRV_Y, DRV_W, DRV_H = 10.5, 2.0, 6.0, 5.0
    drv = elm.Ic(pins=[
        elm.IcPin(name='PWM_L', side='left', slot='4/4'), elm.IcPin(name='DIR_L', side='left', slot='3/4'),
        elm.IcPin(name='PWM_R', side='left', slot='2/4'), elm.IcPin(name='DIR_R', side='left', slot='1/4'),
        elm.IcPin(name='M1A', side='right', slot='4/4'), elm.IcPin(name='M1B', side='right', slot='3/4'),
        elm.IcPin(name='M2A', side='right', slot='2/4'), elm.IcPin(name='M2B', side='right', slot='1/4'),
    ], w=DRV_W, h=DRV_H, plblsize=11, leadlen=1.1).label('DRIVER 2× 20 A', loc='top', fontsize=10)
    drv.right(); drv.anchor('center'); drv.at((DRV_X, DRV_Y)); d.add(drv)
    d += elm.Line().up().at((DRV_X, DRV_Y + DRV_H / 2)).length(0.7); d += elm.Vdd().label('+12V_MOT (VB+)')
    d += elm.Label().label('board logic fed from +12V_LOG', fontsize=7, color=NET).at((DRV_X, DRV_Y + DRV_H / 2 + 1.6))
    d += elm.Line().down().at((DRV_X, DRV_Y - DRV_H / 2)).length(0.7); d += elm.Ground()
    mL = header(d, 18.0, 3.4, 'FRONT MOTOR L', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M1A')).tox(P(mL, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M1B')).tox(P(mL, 'pin2')[0])
    mR = header(d, 18.0, 0.4, 'FRONT MOTOR R', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M2A')).tox(P(mR, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M2B')).tox(P(mR, 'pin2')[0])
    d += elm.Label().label('rear caster wheel: free (not motorized)', fontsize=8).at((15.5, -1.2))

    # ───────────────────────── WS2812 ─────────────────────────
    hw = header(d, 8.5, -3.6, 'CONN WS2812B', ['DIN', '+5V', 'GND'])
    flag(d, P(hw, 'pin1'), 'WS', 'left')
    flag(d, P(hw, 'pin2'), '+5V', 'right')
    d += elm.Ground().at(P(hw, 'pin3'))

    # POWER_HOLD → latch
    flag(d, (5.5, -6.4), 'PWR_HOLD', 'left')
    d += elm.Label().label("→ relay module IN (see power_rails.png)", fontsize=9, color=NET).at((9.9, -6.4))

    # ───────────────────────── Power (top) ─────────────────────────
    ba = header(d, -2.5, 11.0, 'BATTERY 12V', ['+', '-'])
    d += elm.Line().right().at(P(ba, 'pin1')).length(1.0)
    d += elm.Fuse().right().label('40 A', fontsize=8, loc='bottom').length(2.0)
    d += elm.Dot(); rail = d.here
    d += elm.Label().label('→ relay module (+12V_LOG) and 40 A relay + e-stop (+12V_MOT): see power_rails.png',
                           fontsize=8, color=NET).at((rail[0] + 3.4, 12.3))
    d += elm.Line().right().at(rail).length(1.4)
    buck = elm.Ic(pins=[elm.IcPin(name='IN', side='left'), elm.IcPin(name='5V', side='right')],
                  w=2.4, h=1.8, plblsize=9).label('BUCK 12→5V — ≥2 A', loc='top', fontsize=9).right().at((4.2, 10.5)).anchor('IN')
    d.add(buck)
    flag(d, P(buck, 'IN'), '+12V_LOG', 'left')
    flag(d, P(buck, '5V'), '+5V', 'right')
    d += elm.Label().label('(+3V3 = ESP32 board regulator)', fontsize=8).at((4.5, 9.0))

    d += elm.Label().label('Power switching detail (relay module + 40 A relay + e-stop + GPIO22 sense): see power_rails.png',
                           fontsize=10).at((0, -8))

print('render OK')
