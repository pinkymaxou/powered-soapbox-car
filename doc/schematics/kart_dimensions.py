# kart_dimensions.py — Dimensioned drawing of the kart (side view + top view), mm.
# Dimensions = doc/cad/kart_concept.scad (concept). Regenerate:
#   . .venv-schem/bin/activate && python doc/schematics/kart_dimensions.py
#
# REVERSED TRICYCLE (2026-08-06): ONE free 10" caster at the FRONT CENTRE + 2 driven 10"
# wheels at the REAR, under the bench, with the battery in the nose over the caster.
# Reference: x = 0 at the caster, +x toward the rear. Why this way round:
#   · a tricycle tips about the line from the SINGLE wheel to one of the paired wheels, so the
#     usable half-track is (distance CG->single wheel)/wheelbase. Putting the mass over the
#     PAIRED (driven) axle takes that from 45 % to 80 %: 0.39 g -> 0.69 g, same three wheels
#   · the same move puts the load on the driven wheels — traction and braking both act there
#     — and the battery over the caster keeps that wheel planted
# ⚠️ 2026-08-10 (build decision): the bench moved 6" FORWARD and the axle 6" BACK — 12" of
# separation between the passengers and the axle they used to sit on.
# w_eff 332 -> 254 mm, a_tip 0.69 g -> 0.53 g, load on the driven wheels 79 % -> 61 %.
# The software turn limiter stopped being redundancy at that point — with it off the
# simulation now rolls the kart over (see firmware/test_host/sim_main.cpp).
# The deck hangs 4" of shim above the driven axle and the 10" wheels poke through side
# NOTCHES beside it, bearing laterally on the two inboard 2x3 rails — which sets the track.
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle, Polygon
import math

# ── Dimensions (mm) ──
DRIVE_D, WHEEL_W = 254, 70             # driven wheels: 10"
CASTER_D = 254                         # single front caster: 10"
CASTER_PLATE = CASTER_D + 78           # mounting plate height, fork included = 332
PLAT_W, PLAT_L = 30 * 25.4, 46 * 25.4  # 30" x 46" plywood deck: 762 x 1168
TRACK = PLAT_W + WHEEL_W               # 832 — the deck sets the track (notches + 2x3 rails)
EXT = 150                              # caster beam ahead of the deck
DECK_X0 = EXT                          # deck front edge
DECK_X1 = EXT + PLAT_L                 # deck rear edge: 1318
SHIFT_6IN = 6 * 25.4                   # the 2026-08-10 move: 6" each way
DRIVE_X = 1013 + SHIFT_6IN             # 1165 — driven axle went BACK 6" (deck did not move)
CLEAR, LU_H, LU_W, PLY = 80, 64, 38, 12.7
RISER = 102                            # 4" shim between the deck and the axle carriers
FLOOR_Z = CLEAR + RISER + LU_H         # 246 — top of the 2x3 rails
FLOOR_TOP = FLOOR_Z + PLY              # ~259 — deck surface
SEAT_TOP = FLOOR_TOP + 30 + PLY        # ~302 — cushion top
BACK_TOP = FLOOR_TOP + 340 * math.sin(math.radians(82))
GUARD_TOP = FLOOR_TOP + 300
DRIVE_Z, CASTER_Z = DRIVE_D / 2, CASTER_D / 2
# The bench went FORWARD 6": it used to sit ON the driven axle. The 12" of separation this
# opens up with the axle is what costs 78 mm of w_eff.
SEAT_X0, SEAT_X1 = DECK_X1 - 300 - SHIFT_6IN, DECK_X1 - SHIFT_6IN
GUARD_X0 = SEAT_X0 - 70
BACK_X = DECK_X1 - 12 - SHIFT_6IN             # seatback follows the bench forward
BAT_X0, BAT_L, BAT_W, BAT_H = DECK_X0 + 25, 150, 88, 105   # battery in the nose = counterweight
OVERALL_W = TRACK + WHEEL_W
TOTAL_L = DECK_X1 - (-DRIVE_D / 2 + 0) + 0    # placeholder, recomputed below
TOTAL_L = DECK_X1 + 0 - (0 - CASTER_D / 2)    # nose of the caster wheel → deck rear edge
SWEEP_R = max(DRIVE_X + TRACK / 2, DECK_X1)   # pivot envelope: rotation about the driven axle

WOOD, PLYC, DARK, ACC = '#c9a066', '#e8d3a3', '#555555', '#1565c0'

def dim(ax, p0, p1, txt, off=0.0, vert=False, fs=8.5):
    """Dimension with double arrow; off = perpendicular offset."""
    (x0, y0), (x1, y1) = p0, p1
    if vert:
        x0 += off; x1 += off
        ax.annotate('', xy=(x1, y1), xytext=(x0, y0), arrowprops=dict(arrowstyle='<->', lw=1, color=ACC))
        ax.plot([p0[0], x0], [y0, y0], lw=0.5, color=ACC); ax.plot([p1[0], x1], [y1, y1], lw=0.5, color=ACC)
        ax.text(x0 - 14, (y0 + y1) / 2, txt, fontsize=fs, color=ACC, rotation=90, va='center', ha='right')
    else:
        y0 += off; y1 += off
        ax.annotate('', xy=(x1, y1), xytext=(x0, y0), arrowprops=dict(arrowstyle='<->', lw=1, color=ACC))
        ax.plot([x0, x0], [p0[1], y0], lw=0.5, color=ACC); ax.plot([x1, x1], [p1[1], y1], lw=0.5, color=ACC)
        ax.text((x0 + x1) / 2, y0 + 12, txt, fontsize=fs, color=ACC, ha='center')

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12.5, 12))
fig.suptitle("Reversed tricycle — dimensioned drawing (mm) · 30″×46″ deck · 1 free 10″ caster FRONT + 2 driven 10″ REAR",
             fontsize=13, fontweight='bold')

# ═════════════════ SIDE VIEW ═════════════════
ax = ax1
ax.set_title("Side view — free caster left (battery over it), driven wheels right (under the bench)", fontsize=10)
ax.axhline(0, color='k', lw=1)
ax.add_patch(Circle((0, CASTER_Z), CASTER_D / 2, fc=DARK, ec='k'))              # 10" caster
ax.add_patch(Circle((0, CASTER_Z), CASTER_D * 0.27, fc='#bbbbbb', ec='k'))
ax.add_patch(Circle((DRIVE_X, DRIVE_Z), DRIVE_D / 2, fc=DARK, ec='k'))          # 10" driven
ax.add_patch(Circle((DRIVE_X, DRIVE_Z), DRIVE_D * 0.27, fc='#bbbbbb', ec='k'))
ax.add_patch(Rectangle((DECK_X0, CLEAR + RISER), PLAT_L, LU_H, fc=WOOD, ec='k'))   # 2x3 rails
ax.add_patch(Rectangle((DECK_X0, FLOOR_Z), PLAT_L, PLY, fc=PLYC, ec='k'))          # 30"x46" deck
ax.add_patch(Rectangle((0 - 60, FLOOR_Z), EXT + 60, PLY, fc='#d8c090', ec='k'))    # caster nose
# caster post: from the deck up to the 10" caster's mounting plate (73 mm pad)
ax.add_patch(Rectangle((-45, CASTER_PLATE), 90, PLY, fc=PLYC, ec='k'))             # caster plate
ax.add_patch(Rectangle((-6, CASTER_Z - 10), 12, CASTER_PLATE - CASTER_Z + 10, fc='#777777', ec='k'))
ax.add_patch(Rectangle((-30, FLOOR_TOP), 60, CASTER_PLATE - FLOOR_TOP, fc=WOOD, ec='k'))  # short post
# 2" shim stack over the driven axle
ax.add_patch(Rectangle((DRIVE_X - 45, DRIVE_Z + 20), 90, CLEAR + RISER - DRIVE_Z - 20,
                       fc='#8d6e63', ec='k'))
ax.add_patch(Rectangle((BAT_X0, FLOOR_TOP), BAT_L, BAT_H, fc='#222222', ec='k'))   # battery (nose)
ax.add_patch(Rectangle((SEAT_X0, FLOOR_TOP), 300, 30 + PLY, fc=WOOD, ec='k'))      # bench
ax.add_patch(Polygon([(BACK_X, FLOOR_TOP), (BACK_X + 340 * math.cos(math.radians(98)), BACK_TOP),
                      (BACK_X - 12 + 340 * math.cos(math.radians(98)), BACK_TOP), (BACK_X - 12, FLOOR_TOP)],
                     closed=True, fc=WOOD, ec='k'))                                # seatback (leans back)
ax.add_patch(Rectangle((GUARD_X0, FLOOR_TOP), 370, 300, fc=PLYC, ec='k', alpha=0.55))
ax.add_patch(Rectangle((DRIVE_X - 90, CLEAR + RISER + 20), 180, 150, fc='#708090', ec='k'))  # gearbox
ax.add_patch(Rectangle((DRIVE_X - 175, CLEAR + RISER + 110), 80, 42, fc='#888888', ec='k'))  # motor
be_x = BACK_X + 340 * math.cos(math.radians(98))
ax.add_patch(Rectangle((be_x - 22, BACK_TOP), 45, 26, fc='red', ec='k'))           # e-stop
ax.annotate("12 V battery, nose, OVER the caster:\nkeeps that single wheel planted (~20 % of the load)",
            xy=(BAT_X0 + BAT_L / 2, FLOOR_TOP + BAT_H), xytext=(120, 640),
            fontsize=8.5, arrowprops=dict(arrowstyle='->', lw=0.8))
ax.annotate("the 10″ caster needs its plate at 332 mm:\na short pad (73 mm) over the deck",
            xy=(60, CASTER_PLATE + 10), xytext=(230, 500), fontsize=8, color='#b71c1c',
            arrowprops=dict(arrowstyle='->', lw=0.8, color='#b71c1c'))
ax.annotate("bench 6″ AHEAD of the driven axle (axle went back 6″):\nthe 12″ gap costs 78 mm of w_eff — a_tip 0.69 g → 0.53 g,\nso the turn limiter is now load-bearing, not redundancy",
            xy=(SEAT_X0 + 150, FLOOR_TOP + 45), xytext=(440, 570),
            fontsize=8.5, color='#b71c1c',
            arrowprops=dict(arrowstyle='->', lw=0.8, color='#b71c1c'))

dim(ax, (0, 0), (DRIVE_X, 0), f"wheelbase {DRIVE_X} (caster → driven axle)", off=-95)
dim(ax, (-CASTER_D / 2, 0), (DECK_X1, 0), f"overall length ≈ {TOTAL_L:.0f}", off=-170)
dim(ax, (DRIVE_X + 330, 0), (DRIVE_X + 330, DRIVE_D), "Ø254 (10″ driven)", off=0, vert=True)
dim(ax, (-190, 0), (-190, CASTER_D), "Ø254 (10″ caster)", off=0, vert=True, fs=8)
dim(ax, (620, 0), (620, CLEAR + RISER), f"clearance {CLEAR+RISER}", off=0, vert=True, fs=8)
dim(ax, (DRIVE_X + 155, DRIVE_Z + 20), (DRIVE_X + 155, CLEAR + RISER), '4″ shim', off=0, vert=True, fs=8)
dim(ax, (790, 0), (790, SEAT_TOP), f"seat ≈ {SEAT_TOP:.0f}", off=0, vert=True, fs=8)
dim(ax, (240, FLOOR_TOP), (240, CASTER_PLATE), f"raised pad {CASTER_PLATE-FLOOR_TOP:.0f}", off=0, vert=True, fs=8)
ax.set_xlim(-330, DECK_X1 + 430); ax.set_ylim(-260, 700)
ax.set_aspect('equal'); ax.axis('off')

# ═════════════════ TOP VIEW ═════════════════
ax = ax2
ax.add_patch(Rectangle((DECK_X0, -PLAT_W / 2), PLAT_L, PLAT_W, fc=PLYC, ec='k'))     # deck
ax.add_patch(Rectangle((-40, -180), EXT + 40, 360, fc='#d8c090', ec='k'))            # caster nose
for sy in (-1, 1):   # the two inboard 2x3 rails the driven wheels bear against
    ax.add_patch(Rectangle((DECK_X0, sy * PLAT_W / 2 - (LU_W if sy > 0 else 0)), PLAT_L, LU_W,
                           fc=WOOD, ec='k'))
    ax.add_patch(Rectangle((DRIVE_X - DRIVE_D / 2, sy * TRACK / 2 - WHEEL_W / 2),
                           DRIVE_D, WHEEL_W, fc=DARK, ec='k'))                       # driven wheels

ax.add_patch(Rectangle((-CASTER_D / 2, -WHEEL_W / 2), CASTER_D, WHEEL_W, fc='#666666', ec='k'))  # caster
ax.add_patch(Rectangle((SEAT_X0, -PLAT_W / 2), 300, PLAT_W, fc=WOOD, ec='k'))        # bench
ax.add_patch(Rectangle((BAT_X0, -BAT_W / 2), BAT_L, BAT_W, fc='#222222', ec='k'))    # battery
ax.plot([DRIVE_X], [0], marker='+', ms=12, color='k')
ax.annotate("ONE free 10″ caster,\nfront centre (pivots 360°)", xy=(0, 0),
            xytext=(-150, 560), fontsize=8.5, arrowprops=dict(arrowstyle='->', lw=0.8))
ax.annotate("bench 762 (2 children)\n6″ ahead of the driven axle", xy=(SEAT_X0 + 150, 200),
            xytext=(DECK_X1 - 120, 640), fontsize=8.5, arrowprops=dict(arrowstyle='->', lw=0.8))

dim(ax, (DRIVE_X + 210, -TRACK / 2), (DRIVE_X + 210, TRACK / 2), f"track {TRACK:.0f}", off=0, vert=True)
dim(ax, (DECK_X0 + 40, -PLAT_W / 2), (DECK_X0 + 40, PLAT_W / 2), f'deck 30" = {PLAT_W:.0f}', off=0, vert=True)
dim(ax, (DECK_X0, -PLAT_W / 2), (DECK_X1, -PLAT_W / 2), f'deck 46" = {PLAT_L:.0f}', off=-80)
dim(ax, (0, -TRACK / 2 - WHEEL_W / 2), (DRIVE_X, -TRACK / 2 - WHEEL_W / 2), f"wheelbase {DRIVE_X}", off=-150)
ax.text((DECK_X1) / 2, -560,
        "spin in place: rotation about the middle of the DRIVEN axle (+) — the front caster swings around it",
        fontsize=9, style='italic', ha='center')
ax.set_xlim(-330, DECK_X1 + 430); ax.set_ylim(-640, 700)
ax.set_aspect('equal'); ax.axis('off')

fig.tight_layout(rect=[0, 0, 1, 0.97])
fig.savefig('doc/schematics/kart_dimensions.png', dpi=150)
print('render OK')
