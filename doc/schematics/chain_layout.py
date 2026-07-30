# chain_layout.py — Where to put the gearbox output sprocket relative to the wheel one.
# #35 chain, 25T (gearbox) → 32T (wheel). See doc/reducteur.md. Regenerate:
#   . .venv-schem/bin/activate && python doc/schematics/chain_layout.py
import math

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Wedge, FancyArrowPatch

P = 9.525                      # #35 pitch (mm)
R25 = P / (2 * math.sin(math.pi / 25))   # 38.0 mm pitch radius
R32 = P / (2 * math.sin(math.pi / 32))   # 48.6 mm
C = 165.0                      # chosen centre distance (mm) — short, fits the frame

BAD, GOOD, CHAIN, DIM = '#d64545', '#2e9e5b', '#333333', '#777777'


def sprocket(ax, xy, r, label, color='#c9a227'):
    ax.add_patch(Circle(xy, r, fc=color, ec='#7a6316', lw=1.5, zorder=3))
    ax.add_patch(Circle(xy, r * 0.22, fc='white', ec='#7a6316', lw=1.2, zorder=4))
    ax.text(xy[0], xy[1] - r - 13, label, ha='center', va='top', fontsize=9, zorder=5)


def chain_runs(ax, a, ra, b, rb, sag_dir, sag, style='-'):
    """Two tangent runs; the SLACK one bows by `sag` along sag_dir (unit vector)."""
    ang = math.atan2(b[1] - a[1], b[0] - a[0])
    for side in (+1, -1):
        n = (math.cos(ang + side * math.pi / 2), math.sin(ang + side * math.pi / 2))
        p0 = (a[0] + n[0] * ra, a[1] + n[1] * ra)
        p1 = (b[0] + n[0] * rb, b[1] + n[1] * rb)
        # the run whose normal points along sag_dir is the slack one
        slack = (n[0] * sag_dir[0] + n[1] * sag_dir[1]) > 0.3
        if slack and sag > 0:
            mx = (p0[0] + p1[0]) / 2 + sag_dir[0] * sag
            my = (p0[1] + p1[1]) / 2 + sag_dir[1] * sag
            ax.plot([p0[0], mx, p1[0]], [p0[1], my, p1[1]], style,
                    color=CHAIN, lw=2.4, zorder=2, solid_capstyle='round')
        else:
            ax.plot([p0[0], p1[0]], [p0[1], p1[1]], style, color=CHAIN, lw=2.4, zorder=2)


fig, axes = plt.subplots(1, 3, figsize=(15, 6.4))
fig.suptitle("#35 chain, 25T gearbox → 32T wheel — where to put the gearbox output",
             fontsize=13, fontweight='bold')

# ── 1. VERTICAL: what the user correctly predicted ──
ax = axes[0]
wheel, gear = (0, 0), (0, C)
sprocket(ax, wheel, R32, "32T wheel")
sprocket(ax, gear, R25, "25T gearbox")
chain_runs(ax, wheel, R32, gear, R25, (1, 0), 11)
ax.add_patch(FancyArrowPatch((-72, C * 0.62), (-72, 20), arrowstyle='-|>',
                             mutation_scale=16, color=BAD, lw=2.2))
ax.text(-80, C * 0.40, "gravity pulls the\nslack DOWN the\nrun, not into\nthe teeth",
        ha='right', va='center', fontsize=8.5, color=BAD)
ax.text(0, -78, "all the wear slack ends up at the BOTTOM sprocket,\nwhere nothing holds the chain in\n→ it rides up the teeth and skips",
        ha='center', va='top', fontsize=9, color=BAD)
ax.set_title("✗ VERTICAL — avoid", color=BAD, fontweight='bold', pad=16)
ax.set_xlim(-150, 120); ax.set_ylim(-125, C + 78)

# ── 2. HORIZONTAL: the recommendation ──
ax = axes[1]
wheel, gear = (0, 0), (C, 0)
sprocket(ax, wheel, R32, "32T wheel")
sprocket(ax, gear, R25, "25T gearbox")
chain_runs(ax, wheel, R32, gear, R25, (0, -1), 9)
ax.text(C / 2, 78, "tight side on top", ha='center', fontsize=9, color=GOOD)
ax.annotate("", xy=(0, -92), xytext=(C, -92),
            arrowprops=dict(arrowstyle='<->', color=DIM, lw=1.1))
ax.text(C / 2, -89, f"centre distance {C:.0f} mm", ha='center', va='bottom', fontsize=8, color=DIM)
ax.text(C / 2, -102, "slack sags DOWNWARD, clear of everything —\nand gravity keeps the chain seated on BOTH sprockets",
        ha='center', va='top', fontsize=9, color=GOOD)
ax.set_title("✓ HORIZONTAL — do this", color=GOOD, fontweight='bold', pad=16)
ax.set_xlim(-75, C + 75); ax.set_ylim(-150, 100)

# ── 3. The usable ARC: it is a distance, not a point ──
ax = axes[2]
wheel = (0, 0)
RB = C + 55
for a0, a1 in ((-45, 45), (135, 225)):
    ax.add_patch(Wedge(wheel, RB, a0, a1, fc=GOOD, alpha=.18, zorder=0))
for a0, a1 in ((45, 135), (225, 315)):
    ax.add_patch(Wedge(wheel, RB, a0, a1, fc=BAD, alpha=.15, zorder=0))
ax.add_patch(Circle(wheel, C, fill=False, ls='--', lw=1.4, ec=DIM, zorder=1))
sprocket(ax, wheel, R32, "")
for ang in (0, 32, -38, 180, 145, -150):
    r = math.radians(ang)
    sprocket(ax, (C * math.cos(r), C * math.sin(r)), R25, '', color='#dcc46a')
ax.text(0, RB - 26, "✗ within 45° of vertical", ha='center', fontsize=10,
        color=BAD, fontweight='bold')
ax.text(0, -RB + 14, "✓ any position on the dashed circle,\nwithin 45° of horizontal",
        ha='center', fontsize=10, color=GOOD, fontweight='bold')
ax.text(0, -18, "32T\nwheel", ha='center', va='top', fontsize=8.5)
ax.annotate("", xy=(C * 0.72, C * 0.72 * 0.36), xytext=(0, 0),
            arrowprops=dict(arrowstyle='->', color=DIM, lw=1.2))
ax.text(C * 0.40, C * 0.22, f"{C:.0f} mm", fontsize=8.5, color=DIM,
        rotation=20, ha='center', va='bottom')
ax.set_title("The constraint is the DISTANCE, not the direction",
             fontweight='bold', pad=16)
ax.set_xlim(-RB - 12, RB + 12); ax.set_ylim(-RB - 12, RB + 12)

for ax in axes:
    ax.set_aspect('equal')
    ax.axis('off')

LINKS = math.ceil(2*C/P + (25+32)/2 + ((32-25)/(2*math.pi))**2 * P/C)
LINKS += LINKS % 2      # an odd count needs a cranked link: ~20 % weaker
fig.text(0.5, 0.015,
         f"25T Ø{2*R25:.0f} mm · 32T Ø{2*R32:.0f} mm · centre distance {C:.0f} mm → {LINKS} links (even!) · "
         f"slack ~1 % of C ≈ {C*0.01:.1f} mm at mid-span (low, because reverse makes both runs tight in turn) · "
         "slot the mount ±15 mm",
         ha='center', fontsize=8.5, color='#444')
fig.tight_layout(rect=[0, 0.04, 1, 0.95])
fig.savefig('doc/schematics/chain_layout.png', dpi=130)
print("doc/schematics/chain_layout.png written")
