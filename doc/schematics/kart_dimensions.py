# kart_dimensions.py — Plan coté du kart (vue de côté + vue de dessus), mm.
# Cotes = doc/cad/kart_concept.scad (concept). Régénérer :
#   . .venv-schem/bin/activate && python doc/schematics/kart_dimensions.py
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle, Polygon
import math

# ── Cotes (mm) — identiques au concept OpenSCAD ──
WHEEL_D, WHEEL_W = 254, 70
TRACK, PIVOT_X   = 840, 1090          # voie av. ; axe de pivot roulette (empattement)
TRAIL            = 55                  # chasse de la roulette
CLEAR, LU_H, PLY = 80, 64, 12.7
FLOOR_Z = CLEAR + LU_H                 # 144 — dessus longerons
FLOOR_TOP = FLOOR_Z + PLY              # ~157
SEAT_TOP = FLOOR_TOP + 30 + PLY        # ~200 (cales 30 + CP 1/2")
BACK_TOP = FLOOR_TOP + 340 * math.sin(math.radians(82))  # ~494
GUARD_TOP = FLOOR_TOP + 300            # ~457
CASTER_PLATE = FLOOR_TOP + 175         # ~332 (sous-face plateforme)
AXLE_Z = WHEEL_D / 2
CASTER_WHEEL_X = PIVOT_X + TRAIL       # 1145 (fourche alignée)
TOTAL_L = (CASTER_WHEEL_X + WHEEL_D / 2) - (-WHEEL_D / 2)   # ≈ 1399
BODY_W, SEAT_W, BAY_W = 600, 800, 820
OVERALL_W = TRACK + WHEEL_W            # 910

WOOD, PLYC, DARK, ACC = '#c9a066', '#e8d3a3', '#555555', '#1565c0'

def dim(ax, p0, p1, txt, off=0.0, vert=False, fs=8.5):
    """Cote avec double flèche ; off = déport perpendiculaire."""
    (x0, y0), (x1, y1) = p0, p1
    if vert:
        x0 += off; x1 += off
        ax.annotate('', xy=(x1, y1), xytext=(x0, y0), arrowprops=dict(arrowstyle='<->', lw=1, color=ACC))
        ax.plot([p0[0], x0], [y0, y0], lw=0.5, color=ACC); ax.plot([p1[0], x1], [y1, y1], lw=0.5, color=ACC)
        ax.text(x0 + 14, (y0 + y1) / 2, txt, fontsize=fs, color=ACC, rotation=90, va='center')
    else:
        y0 += off; y1 += off
        ax.annotate('', xy=(x1, y1), xytext=(x0, y0), arrowprops=dict(arrowstyle='<->', lw=1, color=ACC))
        ax.plot([x0, x0], [p0[1], y0], lw=0.5, color=ACC); ax.plot([x1, x1], [p1[1], y1], lw=0.5, color=ACC)
        ax.text((x0 + x1) / 2, y0 + 12, txt, fontsize=fs, color=ACC, ha='center')

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12.5, 12))
fig.suptitle("Kart tricycle — plan coté (mm) · roues 10″ · bois 2×3 + CP 1/2″",
             fontsize=13, fontweight='bold')

# ═════════════════ VUE DE CÔTÉ ═════════════════
ax = ax1
ax.set_title("Vue de côté", fontsize=10)
ax.axhline(0, color='k', lw=1)                                             # sol
ax.add_patch(Circle((0, AXLE_Z), WHEEL_D / 2, fc=DARK, ec='k'))            # roue avant
ax.add_patch(Circle((0, AXLE_Z), WHEEL_D * 0.27, fc='#bbbbbb', ec='k'))
ax.add_patch(Circle((CASTER_WHEEL_X, AXLE_Z), WHEEL_D / 2, fc=DARK, ec='k'))  # roulette
ax.add_patch(Circle((CASTER_WHEEL_X, AXLE_Z), WHEEL_D * 0.27, fc='#bbbbbb', ec='k'))
ax.add_patch(Rectangle((20, CLEAR), 1010, LU_H, fc=WOOD, ec='k'))          # longeron
ax.add_patch(Rectangle((20, FLOOR_Z), 1010, PLY, fc=PLYC, ec='k'))         # plancher
ax.add_patch(Rectangle((650, FLOOR_TOP), 300, 30 + PLY, fc=WOOD, ec='k'))  # assise
ax.add_patch(Polygon([(950, FLOOR_TOP), (950 + 340 * math.cos(math.radians(82)), BACK_TOP),
                      (962 + 340 * math.cos(math.radians(82)), BACK_TOP), (962, FLOOR_TOP)],
                     closed=True, fc=WOOD, ec='k'))                        # dossier
ax.add_patch(Rectangle((580, FLOOR_TOP), 400, 300, fc=PLYC, ec='k', alpha=0.55))  # garde-corps
ax.add_patch(Rectangle((PIVOT_X - 60, FLOOR_TOP), 38, 175, fc=WOOD, ec='k'))       # montant queue
ax.add_patch(Rectangle((PIVOT_X - 80, CASTER_PLATE), 160, PLY, fc=PLYC, ec='k'))   # plateforme pivot
ax.add_patch(Rectangle((PIVOT_X - 6, AXLE_Z - 10, ), 12, CASTER_PLATE - AXLE_Z + 10, fc='#777777', ec='k'))  # pivot/fourche
ax.add_patch(Rectangle((-60, CLEAR + 20), 180, 170, fc='#708090', ec='k'))          # boîte
ax.add_patch(Rectangle((115, CLEAR + 130), 80, 42, fc='#888888', ec='k'))           # moteur
be_x = 950 + 340 * math.cos(math.radians(82))
ax.add_patch(Rectangle((be_x - 22, BACK_TOP), 45, 26, fc='red', ec='k'))            # e-stop

dim(ax, (0, 0), (PIVOT_X, 0), "empattement 1090 (essieu → pivot)", off=-95)
dim(ax, (-WHEEL_D / 2, 0), (CASTER_WHEEL_X + WHEEL_D / 2, 0), f"longueur totale ≈ {TOTAL_L:.0f}", off=-170)
dim(ax, (-190, 0), (-190, WHEEL_D), "Ø254 (10″)", off=0, vert=True)
dim(ax, (520, 0), (520, CLEAR), "garde 80", off=0, vert=True, fs=8)
dim(ax, (430, 0), (430, SEAT_TOP), "assise ≈ 200", off=0, vert=True, fs=8)
dim(ax, (1290, 0), (1290, CASTER_PLATE), "platine ≈ 332", off=0, vert=True)
dim(ax, (1390, 0), (1390, BACK_TOP + 26), f"haut. totale ≈ {BACK_TOP + 26:.0f}", off=0, vert=True)
dim(ax, (580, GUARD_TOP), (980, GUARD_TOP), "garde-corps 400", off=28)
dim(ax, (650, SEAT_TOP + 8), (950, SEAT_TOP + 8), "assise 300", off=20)
ax.set_xlim(-320, 1560); ax.set_ylim(-230, 620)
ax.set_aspect('equal'); ax.axis('off')

# ═════════════════ VUE DE DESSUS ═════════════════
ax = ax2
ax.set_title("Vue de dessus", fontsize=10)
for sy in (-1, 1):
    ax.add_patch(Rectangle((-WHEEL_D / 2, sy * TRACK / 2 - WHEEL_W / 2), WHEEL_D, WHEEL_W, fc=DARK, ec='k'))
ax.add_patch(Rectangle((20, -BAY_W / 2), 310, BAY_W, fc=PLYC, ec='k'))          # baie technique
ax.add_patch(Rectangle((330, -BODY_W / 2), 700, BODY_W, fc=PLYC, ec='k'))       # habitacle
ax.add_patch(Rectangle((650, -SEAT_W / 2), 300, SEAT_W, fc=WOOD, ec='k'))       # banquette (débordante)
for sy in (-1, 1):
    ax.add_patch(Rectangle((580, sy * SEAT_W / 2 - (PLY if sy > 0 else 0)), 400, PLY, fc='#8a5a2b', ec='k'))  # garde-corps
ax.add_patch(Rectangle((950, -SEAT_W / 2), 12, SEAT_W, fc='#8a5a2b', ec='k'))   # dossier
ax.plot(0, 0, marker='+', color='k'); ax.plot(PIVOT_X, 0, marker='+', color='k')
ax.add_patch(Circle((PIVOT_X, 0), 32, fc='#999999', ec='k'))                    # platine pivot
th = math.radians(38)                                                           # roulette braquée (pivot visible)
cwx, cwy = PIVOT_X + TRAIL * math.cos(th), TRAIL * math.sin(th)
ax.add_patch(Rectangle((cwx - WHEEL_D / 2, cwy - WHEEL_W / 2), WHEEL_D, WHEEL_W, fc=DARK, ec='k',
             transform=matplotlib.transforms.Affine2D().rotate_around(cwx, cwy, th) + ax.transData))
ax.annotate("roulette folle\n(pivote à 360°)", xy=(cwx, cwy), xytext=(PIVOT_X + 180, 240),
            fontsize=8.5, arrowprops=dict(arrowstyle='->', lw=0.8))

dim(ax, (0, -TRACK / 2), (0, TRACK / 2), "voie 840", off=-235, vert=True)
dim(ax, (-290, -TRACK / 2 - WHEEL_W / 2), (-290, TRACK / 2 + WHEEL_W / 2), f"largeur hors-tout {OVERALL_W}", off=0, vert=True)
dim(ax, (650, -SEAT_W / 2), (650, SEAT_W / 2), "banquette 800 (2 enfants)", off=-160, vert=True)
dim(ax, (900, -BODY_W / 2), (900, BODY_W / 2), "caisse 600", off=0, vert=True)
dim(ax, (20, BAY_W / 2), (330, BAY_W / 2), "baie techn. 310", off=45)
dim(ax, (650, SEAT_W / 2), (950, SEAT_W / 2), "assise 300", off=95)
dim(ax, (0, -TRACK / 2 - WHEEL_W / 2), (PIVOT_X, -TRACK / 2 - WHEEL_W / 2), "empattement 1090", off=-70)
ax.set_xlim(-420, 1560); ax.set_ylim(-660, 660)
ax.set_aspect('equal'); ax.axis('off')

fig.tight_layout()
fig.savefig('doc/schematics/kart_dimensions.png', dpi=150)
print('render OK')
