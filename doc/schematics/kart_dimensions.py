# kart_dimensions.py — Dimensioned drawing of the kart (side view + top view), mm.
# Dimensions = doc/cad/kart_concept.scad (concept). Regenerate:
#   . .venv-schem/bin/activate && python doc/schematics/kart_dimensions.py
# Reference: x=0 = front axle. The axle is SET BACK 325 mm into the body (nose in front):
# the spin-in-place center moves closer to the vehicle midpoint → reduced sweep radius,
# and the 12 V battery in the nose loads the drive wheels (traction/braking).
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle, Polygon
import math

# ── Dimensions (mm) — identical to the OpenSCAD concept ──
WHEEL_D, WHEEL_W = 254, 70
TRACK   = 840
SHIFT   = 325                          # axle set-back into the body
BODY0   = 20 - SHIFT                   # front of the chassis (nose): −305
PIVOT_X = 1090 - SHIFT                 # caster wheel pivot axis: 765 (wheelbase)
TRAIL   = 55                           # caster wheel trail
CLEAR, LU_H, PLY = 80, 64, 12.7
FLOOR_Z = CLEAR + LU_H                 # 144 — top of stringers
FLOOR_TOP = FLOOR_Z + PLY              # ~157
SEAT_TOP = FLOOR_TOP + 30 + PLY        # ~200 (30 spacers + 1/2" plywood)
BACK_TOP = FLOOR_TOP + 340 * math.sin(math.radians(82))  # ~494
GUARD_TOP = FLOOR_TOP + 300            # ~457
CASTER_PLATE = FLOOR_TOP + 175         # ~332 (underside of platform)
AXLE_Z = WHEEL_D / 2
CASTER_WHEEL_X = PIVOT_X + TRAIL       # 820 (fork aligned)
TOTAL_L = (CASTER_WHEEL_X + WHEEL_D / 2) - BODY0            # ≈ 1252
SEAT_X0, SEAT_X1 = 650 - SHIFT, 950 - SHIFT                 # seat: 325..625
GUARD_X0 = 580 - SHIFT                                      # guardrail: 255..655
BACK_X = 950 - SHIFT                                        # seatback foot: 625
BAT_X0, BAT_L, BAT_W, BAT_H = 40 - SHIFT, 150, 88, 105      # 12 V motorcycle battery (nose, centered)
BODY_W, SEAT_W, BAY_W = 600, 800, 820
OVERALL_W = TRACK + WHEEL_W            # 910
SWEEP_R = CASTER_WHEEL_X + WHEEL_D / 2                      # sweep radius when pivoting ≈ 947

WOOD, PLYC, DARK, ACC = '#c9a066', '#e8d3a3', '#555555', '#1565c0'

def dim(ax, p0, p1, txt, off=0.0, vert=False, fs=8.5):
    """Dimension with double arrow; off = perpendicular offset."""
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
fig.suptitle("Tricycle kart — dimensioned drawing (mm) · 10″ wheels · 2×3 lumber + 1/2″ plywood · axle set back (short turning)",
             fontsize=13, fontweight='bold')

# ═════════════════ SIDE VIEW ═════════════════
ax = ax1
ax.set_title("Side view", fontsize=10)
ax.axhline(0, color='k', lw=1)                                             # ground
ax.add_patch(Circle((0, AXLE_Z), WHEEL_D / 2, fc=DARK, ec='k'))            # front wheel (driven)
ax.add_patch(Circle((0, AXLE_Z), WHEEL_D * 0.27, fc='#bbbbbb', ec='k'))
ax.add_patch(Circle((CASTER_WHEEL_X, AXLE_Z), WHEEL_D / 2, fc=DARK, ec='k'))  # caster wheel
ax.add_patch(Circle((CASTER_WHEEL_X, AXLE_Z), WHEEL_D * 0.27, fc='#bbbbbb', ec='k'))
ax.add_patch(Rectangle((BODY0, CLEAR), 1010, LU_H, fc=WOOD, ec='k'))       # stringer
ax.add_patch(Rectangle((BODY0, FLOOR_Z), 1010, PLY, fc=PLYC, ec='k'))      # floor
ax.add_patch(Rectangle((BAT_X0, FLOOR_TOP), BAT_L, BAT_H, fc='#222222', ec='k'))  # 12 V battery
ax.add_patch(Rectangle((SEAT_X0, FLOOR_TOP), 300, 30 + PLY, fc=WOOD, ec='k'))  # seat
ax.add_patch(Polygon([(BACK_X, FLOOR_TOP), (BACK_X + 340 * math.cos(math.radians(82)), BACK_TOP),
                      (BACK_X + 12 + 340 * math.cos(math.radians(82)), BACK_TOP), (BACK_X + 12, FLOOR_TOP)],
                     closed=True, fc=WOOD, ec='k'))                        # seatback
ax.add_patch(Rectangle((GUARD_X0, FLOOR_TOP), 400, 300, fc=PLYC, ec='k', alpha=0.55))  # guardrail
ax.add_patch(Rectangle((PIVOT_X - 60, FLOOR_TOP), 38, 175, fc=WOOD, ec='k'))       # tail post
ax.add_patch(Rectangle((PIVOT_X - 80, CASTER_PLATE), 160, PLY, fc=PLYC, ec='k'))   # pivot platform
ax.add_patch(Rectangle((PIVOT_X - 6, AXLE_Z - 10), 12, CASTER_PLATE - AXLE_Z + 10, fc='#777777', ec='k'))  # pivot/fork
ax.add_patch(Rectangle((-60, CLEAR + 20), 180, 170, fc='#708090', ec='k'))          # gearbox (at the axle)
ax.add_patch(Rectangle((115, CLEAR + 130), 80, 42, fc='#888888', ec='k'))           # motor
be_x = BACK_X + 340 * math.cos(math.radians(82))
ax.add_patch(Rectangle((be_x - 22, BACK_TOP), 45, 26, fc='red', ec='k'))            # e-stop
ax.annotate("12 V motorcycle battery\n(nose, centered)", xy=(BAT_X0 + BAT_L / 2, FLOOR_TOP + BAT_H),
            xytext=(-430, 400), fontsize=8.5, arrowprops=dict(arrowstyle='->', lw=0.8))

dim(ax, (BODY0, 0), (0, 0), "nose 305", off=-95)
dim(ax, (0, 0), (PIVOT_X, 0), "wheelbase 765 (axle → pivot)", off=-95)
dim(ax, (BODY0, 0), (CASTER_WHEEL_X + WHEEL_D / 2, 0), f"overall length ≈ {TOTAL_L:.0f}", off=-170)
dim(ax, (-420, 0), (-420, WHEEL_D), "Ø254 (10″)", off=0, vert=True)
dim(ax, (240, 0), (240, CLEAR), "clearance 80", off=0, vert=True, fs=8)
dim(ax, (290, 0), (290, SEAT_TOP), "seat ≈ 200", off=0, vert=True, fs=8)
dim(ax, (1090, 0), (1090, CASTER_PLATE), "plate ≈ 332", off=0, vert=True)
dim(ax, (1190, 0), (1190, BACK_TOP + 26), f"total height ≈ {BACK_TOP + 26:.0f}", off=0, vert=True)
dim(ax, (GUARD_X0, GUARD_TOP), (GUARD_X0 + 400, GUARD_TOP), "guardrail 400", off=28)
dim(ax, (SEAT_X0, SEAT_TOP + 8), (SEAT_X1, SEAT_TOP + 8), "seat 300", off=20)
ax.set_xlim(-560, 1330); ax.set_ylim(-230, 620)
ax.set_aspect('equal'); ax.axis('off')

# ═════════════════ TOP VIEW ═════════════════
ax = ax2
ax.set_title("Top view", fontsize=10)
for sy in (-1, 1):
    ax.add_patch(Rectangle((-WHEEL_D / 2, sy * TRACK / 2 - WHEEL_W / 2), WHEEL_D, WHEEL_W, fc=DARK, ec='k'))
ax.add_patch(Rectangle((BODY0, -BAY_W / 2), 310, BAY_W, fc=PLYC, ec='k'))        # technical bay (nose)
ax.add_patch(Rectangle((BODY0 + 310, -BODY_W / 2), 700, BODY_W, fc=PLYC, ec='k'))  # cabin
ax.add_patch(Rectangle((BAT_X0, -BAT_W / 2), BAT_L, BAT_W, fc='#222222', ec='k'))  # 12 V battery
ax.add_patch(Rectangle((SEAT_X0, -SEAT_W / 2), 300, SEAT_W, fc=WOOD, ec='k'))    # bench (overhanging)
for sy in (-1, 1):
    ax.add_patch(Rectangle((GUARD_X0, sy * SEAT_W / 2 - (PLY if sy > 0 else 0)), 400, PLY, fc='#8a5a2b', ec='k'))  # guardrail
ax.add_patch(Rectangle((BACK_X, -SEAT_W / 2), 12, SEAT_W, fc='#8a5a2b', ec='k'))  # seatback
ax.plot(0, 0, marker='+', color='k'); ax.plot(PIVOT_X, 0, marker='+', color='k')
ax.add_patch(Circle((PIVOT_X, 0), 32, fc='#999999', ec='k'))                     # pivot plate
th = math.radians(38)                                                            # caster wheel steered (pivot visible)
cwx, cwy = PIVOT_X + TRAIL * math.cos(th), TRAIL * math.sin(th)
ax.add_patch(Rectangle((cwx - WHEEL_D / 2, cwy - WHEEL_W / 2), WHEEL_D, WHEEL_W, fc=DARK, ec='k',
             transform=matplotlib.transforms.Affine2D().rotate_around(cwx, cwy, th) + ax.transData))
ax.annotate("free caster wheel\n(pivots 360°)", xy=(cwx, cwy), xytext=(PIVOT_X + 180, 260),
            fontsize=8.5, arrowprops=dict(arrowstyle='->', lw=0.8))
ax.annotate("12 V motorcycle battery", xy=(BAT_X0 + BAT_L / 2, BAT_W / 2),
            xytext=(BODY0 - 60, 300), fontsize=8.5, arrowprops=dict(arrowstyle='->', lw=0.8))
ax.text((BODY0 + SWEEP_R) / 2, -570,
        "spin in place: rotation around the axle midpoint (+) → sweep radius "
        f"≈ {SWEEP_R:.0f} mm (the set-back axle shortens the turning)",
        fontsize=8, style='italic', ha='center')

dim(ax, (-370, -TRACK / 2), (-370, TRACK / 2), "track 840", off=0, vert=True)
dim(ax, (-460, -TRACK / 2 - WHEEL_W / 2), (-460, TRACK / 2 + WHEEL_W / 2), f"overall width {OVERALL_W}", off=0, vert=True)
dim(ax, (SEAT_X0 + 50, -SEAT_W / 2), (SEAT_X0 + 50, SEAT_W / 2), "bench 800 (2 children)", off=0, vert=True)
dim(ax, (665, -BODY_W / 2), (665, BODY_W / 2), "body 600", off=0, vert=True)
dim(ax, (BODY0, BAY_W / 2), (BODY0 + 310, BAY_W / 2), "tech bay 310", off=45)
dim(ax, (SEAT_X0, SEAT_W / 2), (SEAT_X1, SEAT_W / 2), "seat 300", off=95)
dim(ax, (0, -TRACK / 2 - WHEEL_W / 2), (PIVOT_X, -TRACK / 2 - WHEEL_W / 2), "wheelbase 765", off=-70)
ax.set_xlim(-620, 1300); ax.set_ylim(-660, 660)
ax.set_aspect('equal'); ax.axis('off')

fig.tight_layout()
fig.savefig('doc/schematics/kart_dimensions.png', dpi=150)
print('render OK')
