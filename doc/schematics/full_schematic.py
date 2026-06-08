# full_schematic.py — Schéma électrique complet du kart (style ports nommés / net labels).
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
    d += elm.Label().label('Kart électrique — schéma (ports nommés : nets de même nom = reliés)',
                           fontsize=15).at((6, 12.5))

    # ───────────────────────── ESP32 (circuit intégré central) ─────────────────────────
    LEFT = [('34', 'THR'), ('39', 'VBAT'), ('18', 'SDA'), ('19', 'SCL'), ('16', 'START'), ('21', 'REV')]
    RIGHT = [('25', 'PWM_L'), ('26', 'DIR_L'), ('32', 'PWM_R'), ('33', 'DIR_R'),
             ('17', 'WS'), ('4', 'REV_LED'), ('13', 'PWR_HOLD')]
    pins = []
    for i, (g, _) in enumerate(LEFT):
        pins.append(elm.IcPin(name=g, side='left', slot=f'{len(LEFT)-i}/{len(LEFT)}'))
    for i, (g, _) in enumerate(RIGHT):
        pins.append(elm.IcPin(name=g, side='right', slot=f'{len(RIGHT)-i}/{len(RIGHT)}'))
    pins += [elm.IcPin(name='3V3', side='top'), elm.IcPin(name='5V', side='top'),
             elm.IcPin(name='GND', side='bottom')]
    esp = elm.Ic(pins=pins, label='ESP32\nWROOM', w=4.6, h=9.5, plblsize=12, leadlen=1.1)
    esp.right(); esp.anchor('center'); esp.at((0, 0)); d.add(esp)
    # Étiquettes de net sur chaque broche (le n° GPIO est sur la broche, le NET sur le drapeau)
    for g, net in LEFT:
        flag(d, P(esp, g), net, 'left')
    for g, net in RIGHT:
        flag(d, P(esp, g), net, 'right')
    d += elm.Line().up().at(P(esp, '3V3')).length(0.9); d += elm.Vdd().label('+3V3')
    d += elm.Line().up().at(P(esp, '5V')).length(0.9); d += elm.Vdd().label('+5V')
    d += elm.Line().down().at(P(esp, 'GND')).length(0.6); d += elm.Ground()

    # ───────────────────────── Accélérateur (diviseur ÷1,5) ─────────────────────────
    hp = header(d, -11.5, 7.0, 'CONN PEDALE', ['SIG', '+5V', 'GND'])
    d += elm.Line().right().at(P(hp, 'pin1')).length(0.6)
    d += (r1 := elm.Resistor().right().label('10k', fontsize=8))
    d += elm.Dot(); nd = d.here
    flag(d, nd, 'THR', 'right')
    d += elm.Resistor().down().at(nd).label('20k', fontsize=8).length(1.5)
    d += elm.Ground()
    d += elm.Capacitor().down().at(nd).length(1.5)   # 0,1 µF (symbolique, en parallèle)
    flag(d, P(hp, 'pin2'), '+5V', 'left')
    d += elm.Ground().at(P(hp, 'pin3'))

    # ───────────────────────── Tension batterie (diviseur 100k/15k) ─────────────────────────
    flag(d, (-11.5, 3.2), '+20V', 'left'); vtop = (-10.6, 3.2)
    d += elm.Resistor().down().at(vtop).label('100k', fontsize=8).length(1.6)
    d += elm.Dot(); nv = d.here
    flag(d, nv, 'VBAT', 'left')
    d += elm.Resistor().down().at(nv).label('15k', fontsize=8).length(1.6)
    d += elm.Ground()

    # ───────────────────────── Capteur AS5600 (I2C + pull-ups) ─────────────────────────
    ha = header(d, -11.5, -1.2, 'CONN AS5600 (essieu)', ['SDA', 'SCL', '3V3', 'GND'])
    flag(d, P(ha, 'pin1'), 'SDA', 'left')
    flag(d, P(ha, 'pin2'), 'SCL', 'left')
    flag(d, P(ha, 'pin3'), '+3V3', 'left')
    d += elm.Ground().at(P(ha, 'pin4'))
    # Pull-ups 4,7 k vers +3V3
    d += elm.Vdd().at((-8.0, 0.6)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-8.0, 0.6)).length(1.4).label('4k7', fontsize=8)
    flag(d, d.here, 'SDA', 'down')
    d += elm.Vdd().at((-7.0, 0.6)).label('+3V3', fontsize=8)
    d += elm.Resistor().down().at((-7.0, 0.6)).length(1.4).label('4k7', fontsize=8)
    flag(d, d.here, 'SCL', 'down')

    # ───────────────────────── Boutons START / REVERSE ─────────────────────────
    hb = header(d, -11.5, -5.0, 'CONN START', ['S', 'GND'])
    flag(d, P(hb, 'pin1'), 'START', 'left'); d += elm.Ground().at(P(hb, 'pin2'))
    hr = header(d, -8.2, -5.0, 'CONN RECUL', ['S', 'GND'])
    flag(d, P(hr, 'pin1'), 'REV', 'left'); d += elm.Ground().at(P(hr, 'pin2'))

    # ───────────────────────── Driver moteur + moteurs ─────────────────────────
    # V+ / GND non mis en broches Ic (évite la collision libellé haut/bas vs rangées) :
    # tirés en talons depuis les bords de la boîte (coordonnées calculées).
    DRV_X, DRV_Y, DRV_W, DRV_H = 10.5, 2.0, 6.0, 5.0
    drv = elm.Ic(pins=[
        elm.IcPin(name='PWM_L', side='left', slot='4/4'), elm.IcPin(name='DIR_L', side='left', slot='3/4'),
        elm.IcPin(name='PWM_R', side='left', slot='2/4'), elm.IcPin(name='DIR_R', side='left', slot='1/4'),
        elm.IcPin(name='M1A', side='right', slot='4/4'), elm.IcPin(name='M1B', side='right', slot='3/4'),
        elm.IcPin(name='M2A', side='right', slot='2/4'), elm.IcPin(name='M2B', side='right', slot='1/4'),
    ], label='DRIVER\n20 A', w=DRV_W, h=DRV_H, plblsize=11, leadlen=1.1)
    drv.right(); drv.anchor('center'); drv.at((DRV_X, DRV_Y)); d.add(drv)
    # Pas de drapeau ici : les noms de broches du driver SONT déjà les nets (PWM_L…).
    d += elm.Line().up().at((DRV_X, DRV_Y + DRV_H / 2)).length(0.7); d += elm.Vdd().label('+20V')
    d += elm.Line().down().at((DRV_X, DRV_Y - DRV_H / 2)).length(0.7); d += elm.Ground()
    mL = header(d, 18.0, 3.4, 'MOTEUR G', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M1A')).tox(P(mL, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M1B')).tox(P(mL, 'pin2')[0])
    mR = header(d, 18.0, 0.4, 'MOTEUR D', ['A', 'B'])
    d += elm.Line().at(P(drv, 'M2A')).tox(P(mR, 'pin1')[0])
    d += elm.Line().at(P(drv, 'M2B')).tox(P(mR, 'pin2')[0])

    # ───────────────────────── WS2812 + LED recul ─────────────────────────
    hw = header(d, 8.5, -3.2, 'CONN WS2812B', ['DIN', '+5V', 'GND'])
    flag(d, P(hw, 'pin1'), 'WS', 'left')
    flag(d, P(hw, 'pin2'), '+5V', 'right')
    d += elm.Ground().at(P(hw, 'pin3'))

    flag(d, (1.5, -5.2), 'REV_LED', 'left')
    d += elm.Line().right().at((2.4, -5.2)).length(0.4)
    d += elm.Resistor().right().label('330', fontsize=8)
    d += elm.LED().right().label('LED recul', fontsize=8)
    d += elm.Ground()

    # POWER_HOLD → latch
    flag(d, (5.5, -6.2), 'PWR_HOLD', 'left')
    d += elm.Label().label('→ latch d\'alimentation (cf. power_latch.png)', fontsize=9, color=NET).at((9.5, -6.2))

    # ───────────────────────── Alimentation (haut) ─────────────────────────
    ba = header(d, -2.5, 10.5, 'PACK A 20V', ['+', '-'])
    d += elm.Diode().right().at(P(ba, 'pin1')).label('idéale', fontsize=8).length(2.0)
    d += elm.Dot(); rail = d.here
    bb = header(d, -2.5, 8.6, 'PACK B 20V', ['+', '-'])
    d += elm.Diode().right().at(P(bb, 'pin1')).label('idéale', fontsize=8).length(2.0)
    d += elm.Line().toy(rail[1]); d += elm.Dot()
    flag(d, rail, '+20V', 'up')
    d += elm.Line().right().at(rail).length(1.4)
    buck = elm.Ic(pins=[elm.IcPin(name='IN', side='left'), elm.IcPin(name='5V', side='right')],
                  label='BUCK\n20→5V', w=2.2, h=1.8, plblsize=9).right().at((4.0, 10.0)).anchor('IN')
    d.add(buck)
    d += elm.Line().left().at(P(buck, 'IN')).tox(rail[0] + 1.4)
    flag(d, P(buck, '5V'), '+5V', 'right')
    d += elm.Label().label('(+3V3 = régulateur de la carte ESP32)', fontsize=8).at((4.5, 8.6))

    d += elm.Label().label('Coupe-circuit détaillé (2× IRFZ44N low-side + opto + e-stop) : voir power_latch.png',
                           fontsize=10).at((0, -8))

print('render OK')
