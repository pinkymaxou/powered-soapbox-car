# power_latch.py — Architecture d'alimentation du kart (latch low-side, 2 chemins).
# Amorcage par bouton (+20 V direct sur la gate) ; maintien par l'ESP via opto (LED cote 3,3 V).
#   . .venv-schem/bin/activate && python doc/schematics/power_latch.py
import schemdraw
import schemdraw.elements as elm

schemdraw.config(fontsize=12, lw=1.9, unit=2.6)

with schemdraw.Drawing(file='doc/schematics/power_latch.png', dpi=170, show=False) as d:
    # ───────── Optocoupleur : LED (gauche, cote 3,3 V) / phototransistor (droite, cote +20 V) ─────────
    opto = d.add(elm.Optocoupler(box=True).right())
    ocx = (opto.anode[0] + opto.collector[0]) / 2
    d += elm.Label().label('OPTOCOUPLEUR\n(isolation)').at((ocx, opto.emitter[1] - 1.1)).color('#1565c0')

    # LED : anode -> R -> +3,3 V (present seulement APRES demarrage)
    d += elm.Resistor().up().at(opto.anode).label('220 Ω')
    d += elm.Vdd().label('+3,3 V (ESP)')

    # LED : cathode -> ESP GPIO13 (actif bas : tire la LED a la masse pour MAINTENIR)
    d += elm.Line().down().at(opto.cathode).length(1.4)
    d += elm.Switch().down().length(1.6)
    esp_end = d.here
    d += elm.Ground()
    d += elm.Label().label('ESP GPIO13\nPOWER_HOLD\n(actif BAS = maintien)').at((esp_end[0], esp_end[1] - 1.3))

    # ───────── Cote +20 V : e-stop -> ligne de gate, 2 chemins (opto OU bouton) ─────────
    # Collecteur de l'opto remonte au noeud E (apres e-stop)
    d += elm.Line().up().at(opto.collector).length(1.2)
    Enode = d.here
    d += elm.Dot()
    d += elm.Switch().up().length(1.7).label('e-stop\n(NF, serie)', loc='right')
    d += elm.Line().up().length(0.5)
    d += elm.Vdd().label('+20 V')

    # Chemin AMORCAGE : E -> bouton -> noeud de merge M
    d += elm.Line().right().at(Enode).length(3.2)
    btn_top = d.here
    d += elm.Button().down().length(1.7).label('Bouton\ndemarrage\n(amorcage)', loc='right')
    Mnode = d.here
    d += elm.Dot()

    # Chemin MAINTIEN : emetteur de l'opto -> descend -> rejoint M
    d += elm.Line().down().at(opto.emitter).length(1.0)
    d += elm.Line().toy(Mnode[1]).color('#000')   # aligne en y
    d += elm.Line().tox(Mnode[0])                  # rejoint M en x

    # M -> Rg serie -> GATE commune
    d += elm.Resistor().right().at(Mnode).length(2.4).label('Rg', loc='bottom')
    gpt = d.here
    d += elm.Dot().label('GATE commune', loc='top', ofst=(0, 0.25)).color('#1565c0')

    # Securites : pull-down + zener
    d += elm.Resistor().down().at(gpt).length(2.6).label('R\npull-down', loc='left', ofst=(-0.1, 0.4))
    d += elm.Ground()
    d += elm.Line().right().at(gpt).length(2.0)
    zpt = d.here
    d += elm.Dot()
    d += elm.Zener().down().at(zpt).length(2.6).label('Zener\n~15 V', loc='left', ofst=(-0.6, 0.9))
    d += elm.Ground()

    # ───────── 2x IRFZ44N en low-side (gate commune) ─────────
    d += elm.Line().right().at(zpt).length(2.4)
    gA = d.here
    q1 = d.add(elm.NFet(bulk=False).right().anchor('gate'))
    d += elm.Label().label('IRFZ44N #1', ofst=(0.2, 0.9)).at(q1.drain).color('#444')
    d += elm.Line().up().at(q1.drain).length(1.2).label('− pack A', loc='right')
    d += elm.Line().down().at(q1.source).length(0.6)
    d += elm.Ground()

    d += elm.Line().down().at(gA).length(3.4)
    gB = d.here
    q2 = d.add(elm.NFet(bulk=False).right().anchor('gate'))
    d += elm.Label().label('IRFZ44N #2', ofst=(0.2, -0.9)).at(q2.source).color('#444')
    d += elm.Line().up().at(q2.drain).length(0.6).label('− pack B', loc='right')
    d += elm.Ground().at(q2.source)

print('render OK')
