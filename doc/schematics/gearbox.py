# gearbox.py — Schéma du réducteur 1:12,5 imprimé (vue en plan + coupe axiale).
# Cotes réelles (24 DP) — voir doc/reducteur.md. Régénérer :
#   . .venv-schem/bin/activate && python doc/schematics/gearbox.py
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle

# ───────────────────────── Cotes (mm) ─────────────────────────
DP = 24.0
IN = 25.4
def pitch_d(z): return z / DP * IN          # Ø primitif
def outer_d(z): return (z + 2) / DP * IN    # Ø extérieur

# Axes (vue en plan, disposition alignée comme le prototype)
A_MOT = (0.0, 0.0)                # axe moteur (pignon 16T)
E1 = (16 + 80) / (2 * DP) * IN    # 50,80
E2 = (32 + 80) / (2 * DP) * IN    # 59,27
A_INT = (E1, 0.0)                 # axe intermédiaire (80T + 32T)
A_OUT = (E1 + E2, 0.0)            # axe de sortie (64T + poulie)

C_PIN, C_64, C_32 = '#888888', '#e6b800', '#e6b800'

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 6.2))
fig.suptitle("Réducteur 1:12,5 — 16T→80T (5:1) puis 32T→80T (2,5:1), denture 24 DP",
             fontsize=13, fontweight='bold')

# ───────────────────────── Vue en plan ─────────────────────────
ax1.set_title("Vue en plan (cercles primitifs en tirets, extérieurs en trait plein)", fontsize=9)
def gear_plan(ax, center, z, color, label, label_dy):
    ax.add_patch(Circle(center, outer_d(z) / 2, fill=True, facecolor=color,
                        edgecolor='black', alpha=0.35, lw=1.2))
    ax.add_patch(Circle(center, pitch_d(z) / 2, fill=False, edgecolor='black',
                        ls='--', lw=0.9))
    ax.plot(*center, marker='+', color='black', ms=10)
    ax.annotate(label, center, textcoords='offset points', xytext=(0, label_dy),
                ha='center', fontsize=8.5)

gear_plan(ax1, A_MOT, 16, C_PIN, "16T moteur\nØp 16,9", -34)
gear_plan(ax1, A_INT, 80, C_64, "80T (ép. 10)\nØp 84,7", 50)
gear_plan(ax1, A_INT, 32, C_32, "32T (ép. 20)\nØp 33,9", -8)
gear_plan(ax1, A_OUT, 80, C_64, "80T sortie\nØp 84,7", 50)

# Entraxes cotés
for (x0, x1, txt) in [(A_MOT[0], A_INT[0], "50,80 (2.0000″)"),
                      (A_INT[0], A_OUT[0], "59,27 (2.3333″)")]:
    y = -46
    ax1.annotate('', xy=(x1, y), xytext=(x0, y), arrowprops=dict(arrowstyle='<->', lw=1))
    ax1.text((x0 + x1) / 2, y - 5, txt, ha='center', fontsize=8.5)

ax1.set_xlim(-30, 160); ax1.set_ylim(-70, 58)
ax1.set_aspect('equal'); ax1.axis('off')

# ───────────────────────── Coupe axiale ─────────────────────────
ax2.set_title("Coupe axiale (empilement, carter 2 coques + roulements 6805)", fontsize=9)
PLATE = 8.0          # épaisseur plaques carter
BRG_T, BRG_D = 7.0, 37.0
Z0 = 0.0             # face intérieure plaque arrière
T64, T32 = 10.0, 20.0
GAP = 1.5            # jeu axial 64T sortie ↔ 64T étage 1

def rect(ax, x, y, w, h, **kw):
    ax.add_patch(Rectangle((x, y), w, h, **kw))

# Plaques (arrière porte-moteur, avant dévissable)
zfront = Z0 + T64 + T32 + GAP + 6
rect(ax2, -22, Z0 - PLATE, 185, PLATE, facecolor='#333333')
rect(ax2, -22, zfront, 185, PLATE, facecolor='#555555', hatch='//', edgecolor='black', lw=0.5)
ax2.text(166, Z0 - PLATE / 2, "plaque arrière\n(+ moteur)", fontsize=7.5, va='center')
ax2.text(166, zfront + PLATE / 2, "plaque avant\ndévissable", fontsize=7.5, va='center')

# Moteur + pignon 16T (6,35 utile) au niveau de la 64T étage 1
rect(ax2, A_MOT[0] - 17, Z0 - PLATE - 28, 34, 28, facecolor='#666666')
ax2.text(A_MOT[0], Z0 - PLATE - 34, "moteur", ha='center', fontsize=8)
rect(ax2, A_MOT[0] - 9.5, Z0 + 1.5, 19, 6.35, facecolor=C_PIN, edgecolor='black')
ax2.text(A_MOT[0], Z0 + 12, "16T\n(6,35)", ha='center', fontsize=7.5)

# Pignon composé : 64T (10) + 32T (20) + tourillons Ø25 → roulements dans les 2 plaques
x = A_INT[0]
rect(ax2, x - 43.4, Z0, 86.78, T64, facecolor=C_64, edgecolor='black', alpha=0.75)
rect(ax2, x - 18.0, Z0 + T64, 36, T32, facecolor=C_64, edgecolor='black', alpha=0.75)
rect(ax2, x - 12.5, Z0 - BRG_T, 25, BRG_T, facecolor='#bbbbbb', edgecolor='black')       # tourillon AR
rect(ax2, x - 12.5, Z0 + T64 + T32, 25, zfront - (Z0 + T64 + T32), facecolor='#bbbbbb', edgecolor='black')
for zb in (Z0 - BRG_T, zfront - 0.001):                                                   # roulements
    rect(ax2, x - BRG_D / 2, zb if zb < Z0 else zfront, BRG_D, BRG_T * (1 if zb < Z0 else -1) * -1 + 0,
         facecolor='none')
rect(ax2, x - BRG_D / 2, Z0 - BRG_T, BRG_D, BRG_T, facecolor='#88aadd', edgecolor='black', alpha=0.6)
rect(ax2, x - BRG_D / 2, zfront - BRG_T, BRG_D, BRG_T, facecolor='#88aadd', edgecolor='black', alpha=0.6)
ax2.text(x, Z0 + T64 / 2, "80T", ha='center', fontsize=8)
ax2.text(x, Z0 + T64 + T32 / 2, "32T", ha='center', fontsize=8)

# 64T de sortie : au niveau de la 32T (décalée de GAP au-dessus de la 64T étage 1)
xo = A_OUT[0]
z64o = Z0 + T64 + GAP
rect(ax2, xo - 43.4, z64o, 86.78, T32 - GAP, facecolor=C_64, edgecolor='black', alpha=0.75)
rect(ax2, xo - 12.5, z64o + (T32 - GAP), 25, zfront - (z64o + T32 - GAP), facecolor='#bbbbbb', edgecolor='black')
rect(ax2, xo - 12.5, Z0 - BRG_T, 25, BRG_T + z64o - Z0, facecolor='#bbbbbb', edgecolor='black')
rect(ax2, xo - BRG_D / 2, Z0 - BRG_T, BRG_D, BRG_T, facecolor='#88aadd', edgecolor='black', alpha=0.6)
rect(ax2, xo - BRG_D / 2, zfront - BRG_T, BRG_D, BRG_T, facecolor='#88aadd', edgecolor='black', alpha=0.6)
ax2.text(xo, z64o + (T32 - GAP) / 2, "80T sortie", ha='center', fontsize=8)
ax2.annotate("jeu ≥1 mm", xy=(xo - 30, Z0 + T64 + GAP / 2), xytext=(xo - 62, Z0 + T64 + 14),
             fontsize=7.5, arrowprops=dict(arrowstyle='->', lw=0.8))
ax2.annotate("roulement 6805\n(25×37×7)", xy=(x - 10, Z0 - BRG_T / 2), xytext=(-25, Z0 - 24),
             fontsize=7.5, arrowprops=dict(arrowstyle='->', lw=0.8))
ax2.annotate("tourillon Ø25\n(vers poulie)", xy=(xo + 6, zfront + PLATE + 1),
             xytext=(xo + 22, zfront + 14), fontsize=7.5,
             arrowprops=dict(arrowstyle='->', lw=0.8))
# Sortie de boîte : le tourillon avant traverse la plaque (poulie côté extérieur)
rect(ax2, xo - 12.5, zfront, 25, PLATE + 8, facecolor='#bbbbbb', edgecolor='black')

ax2.set_xlim(-32, 195); ax2.set_ylim(Z0 - PLATE - 40, zfront + PLATE + 18)
ax2.set_aspect('equal'); ax2.axis('off')

fig.tight_layout()
fig.savefig('doc/schematics/gearbox.png', dpi=160)
print('render OK')
