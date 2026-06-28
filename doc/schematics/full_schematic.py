# full_schematic.py — Schéma électrique du kart (variante EXPÉRIMENTALE : entraînement
# différentiel). 2 moteurs AVANT indépendants, 2 capteurs AS5600 (un par bus I2C), ADC externe
# ADS1115 (Vbat), pilotage par MANETTE BLUETOOTH (radio interne ESP32, aucune pédale).
# Génère doc/schematics/full_schematic.png via schemdraw.
#   . .venv-schem/bin/activate && python doc/schematics/full_schematic.py
#
# Convention : les étiquettes de net (drapeaux) de MÊME NOM sont électriquement reliées
# (comme sur un schéma multi-feuilles). Évite de tirer des fils à travers toute la page.
import schemdraw
import schemdraw.elements as elm

schemdraw.config(fontsize=10, lw=1.7)
NET = '#1565c0'

def P(elem, name):
    # absanchors = coordonnées ABSOLUES (anchors = local, non transformé → à ne pas utiliser ici)
    return elem.absanchors[name]

def flag(d, xy, name, direction):
    """Drapeau de net nommé, pointant dans 'direction'."""
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
    d += elm.Label().label('Kart électrique différentiel — schéma (ports nommés : nets de même nom = reliés)',
                           fontsize=15).at((6, 13.0))

    # ───────────────────────── ESP32 (circuit intégré central) ─────────────────────────
    # 2 bus I2C : bus 0 (SDA0/SCL0 = AS5600 G + ADS1115) · bus 1 (SDA1/SCL1 = AS5600 D).
    LEFT = [('18', 'SDA0'), ('19', 'SCL0'), ('27', 'SDA1'), ('14', 'SCL1'), ('16', 'START')]
    RIGHT = [('25', 'PWM_L'), ('26', 'DIR_L'), ('32', 'PWM_R'), ('33', 'DIR_R'),
             ('17', 'WS'), ('13', 'PWR_HOLD')]
    pins = []
    for i, (g, _) in enumerate(LEFT):
        pins.append(elm.IcPin(name=g, side='left', slot=f'{len(LEFT)-i}/{len(LEFT)}'))
    for i, (g, _) in enumerate(RIGHT):
        pins.append(elm.IcPin(name=g, side='right', slot=f'{len(RIGHT)-i}/{len(RIGHT)}'))
    pins += [elm.IcPin(name='3V3', side='top'), elm.IcPin(name='5V', side='top'),
             elm.IcPin(name='GND', side='bottom')]
    esp = elm.Ic(pins=pins, label='ESP32\nWROOM', w=4.6, h=9.5, plblsize=12, leadlen=1.1)
    esp.right(); esp.anchor('center'); esp.at((0, 0)); d.add(esp)
    d += elm.Label().label('manette BT\n(radio interne)', fontsize=8, color=NET).at((0, -5.6))
    for g, net in LEFT:
        flag(d, P(esp, g), net, 'left')
    for g, net in RIGHT:
        flag(d, P(esp, g), net, 'right')
    d += elm.Line().up().at(P(esp, '3V3')).length(0.9); d += elm.Vdd().label('+3V3')
    d += elm.Line().up().at(P(esp, '5V')).length(0.9); d += elm.Vdd().label('+5V')
    d += elm.Line().down().at(P(esp, 'GND')).length(0.6); d += elm.Ground()

    # ───────────── ADC externe ADS1115 (bus 0) + tension batterie sur A0 ─────────────
    ha = header(d, -11.5, 9.0, 'ADS1115 (0x48)', ['A0', 'SDA', 'SCL', '3V3', 'GND'])
    flag(d, P(ha, 'pin1'), 'VBAT', 'left')     # A0 = milieu du diviseur (net VBAT)
    flag(d, P(ha, 'pin2'), 'SDA0', 'left')
    flag(d, P(ha, 'pin3'), 'SCL0', 'left')
    flag(d, P(ha, 'pin4'), '+3V3', 'left')
    d += elm.Ground().at(P(ha, 'pin5'))
    d += elm.Label().label('A1/A2 : joystick FUTUR', fontsize=7).at((-10.2, 6.3))
    # Diviseur 100k/15k : +20V → VBAT → GND (VBAT va vers A0)
    flag(d, (-7.6, 10.4), '+20V', 'up'); vtop = (-7.0, 10.4)
    d += elm.Resistor().down().at(vtop).label('100k', fontsize=8).length(1.4)
    d += elm.Dot(); nv = d.here
    flag(d, nv, 'VBAT', 'right')
    d += elm.Resistor().down().at(nv).label('15k', fontsize=8).length(1.4)
    d += elm.Ground()

    # ───────────── Capteur AS5600 roue GAUCHE (bus 0) + pull-ups ─────────────
    hg = header(d, -11.5, 4.0, 'AS5600 roue G (0x36)', ['SDA', 'SCL', '3V3', 'GND'])
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

    # ───────────── Capteur AS5600 roue DROITE (bus 1) + pull-ups ─────────────
    hd = header(d, -11.5, -0.6, 'AS5600 roue D (0x36)', ['SDA', 'SCL', '3V3', 'GND'])
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

    # ───────────────────────── Bouton START (armement) ─────────────────────────
    hb = header(d, -11.5, -5.0, 'CONN START', ['S', 'GND'])
    flag(d, P(hb, 'pin1'), 'START', 'left'); d += elm.Ground().at(P(hb, 'pin2'))
    d += elm.Label().label("(armement aussi par le bouton START de la manette)", fontsize=7).at((-9.2, -6.4))

    # ───────────────────────── Driver moteur + 2 moteurs AVANT ─────────────────────────
    DRV_X, DRV_Y, DRV_W, DRV_H = 10.5, 2.0, 6.0, 5.0
    drv = elm.Ic(pins=[
        elm.IcPin(name='PWM_L', side='left', slot='4/4'), elm.IcPin(name='DIR_L', side='left', slot='3/4'),
        elm.IcPin(name='PWM_R', side='left', slot='2/4'), elm.IcPin(name='DIR_R', side='left', slot='1/4'),
        elm.IcPin(name='M1A', side='right', slot='4/4'), elm.IcPin(name='M1B', side='right', slot='3/4'),
        elm.IcPin(name='M2A', side='right', slot='2/4'), elm.IcPin(name='M2B', side='right', slot='1/4'),
    ], label='DRIVER\n2× 20 A', w=DRV_W, h=DRV_H, plblsize=11, leadlen=1.1)
    drv.right(); drv.anchor('center'); drv.at((DRV_X, DRV_Y)); d.add(drv)
    d += elm.Line().up().at((DRV_X, DRV_Y + DRV_H / 2)).length(0.7); d += elm.Vdd().label('+20V')
    d += elm.Line().down().at((DRV_X, DRV_Y - DRV_H / 2)).length(0.7); d += elm.Ground()
    mL = header(d, 18.0, 3.4, 'MOTEUR AVANT G', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M1A')).tox(P(mL, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M1B')).tox(P(mL, 'pin2')[0])
    mR = header(d, 18.0, 0.4, 'MOTEUR AVANT D', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M2A')).tox(P(mR, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M2B')).tox(P(mR, 'pin2')[0])
    d += elm.Label().label('roulette arrière : libre (non motorisée)', fontsize=8).at((15.5, -1.2))

    # ───────────────────────── WS2812 ─────────────────────────
    hw = header(d, 8.5, -3.6, 'CONN WS2812B', ['DIN', '+5V', 'GND'])
    flag(d, P(hw, 'pin1'), 'WS', 'left')
    flag(d, P(hw, 'pin2'), '+5V', 'right')
    d += elm.Ground().at(P(hw, 'pin3'))

    # POWER_HOLD → latch
    flag(d, (5.5, -6.4), 'PWR_HOLD', 'left')
    d += elm.Label().label("→ latch d'alimentation (cf. power_latch.png)", fontsize=9, color=NET).at((9.7, -6.4))

    # ───────────────────────── Alimentation (haut) ─────────────────────────
    ba = header(d, -2.5, 11.0, 'PACK A 20V', ['+', '-'])
    d += elm.Diode().right().at(P(ba, 'pin1')).label('idéale', fontsize=8).length(2.0)
    d += elm.Dot(); rail = d.here
    bb = header(d, -2.5, 9.1, 'PACK B 20V', ['+', '-'])
    d += elm.Diode().right().at(P(bb, 'pin1')).label('idéale', fontsize=8).length(2.0)
    d += elm.Line().toy(rail[1]); d += elm.Dot()
    flag(d, rail, '+20V', 'up')
    d += elm.Line().right().at(rail).length(1.4)
    buck = elm.Ic(pins=[elm.IcPin(name='IN', side='left'), elm.IcPin(name='5V', side='right')],
                  label='BUCK\n20→5V', w=2.2, h=1.8, plblsize=9).right().at((4.0, 10.5)).anchor('IN')
    d.add(buck)
    d += elm.Line().left().at(P(buck, 'IN')).tox(rail[0] + 1.4)
    flag(d, P(buck, '5V'), '+5V', 'right')
    d += elm.Label().label('(+3V3 = régulateur de la carte ESP32)', fontsize=8).at((4.5, 9.0))

    d += elm.Label().label('Coupe-circuit détaillé (2× IRFZ44N low-side + opto + e-stop) : voir power_latch.png',
                           fontsize=10).at((0, -8))
    d += elm.Label().label('Réserve câblée (non utilisée) : 2 encodeurs AMT103-V quadrature A/B sur GPIO 34/35 (G) et 36/39 (D).',
                           fontsize=9).at((0, -8.7))

print('render OK')
