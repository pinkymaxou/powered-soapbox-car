# If the driven wheels went back to the front

*Study, not a plan. Answers one question with the repository's own physics model: **if the
driven pair moved back to the FRONT and the free caster to the REAR, how far forward would
the bench have to move to stay stable with two passengers?** Short answer: **~24 cm (9.5″)**
to break even with the kart as built — and that is where the trouble starts, because those
24 cm put the children's feet in the drivetrain.*

## The rule, in one line

A tricycle tips about the line joining its **single** wheel to one of the **paired** ones, so
the usable half-track is what is left of it once the CG has drifted toward the single wheel:

```
w_eff = (track/2) × (1 − x_cg / wheelbase)        a_tip = g × w_eff / h_cg
```

`x_cg` is the distance from the CG to the **paired (driven) axle**. The formula does not care
which end of the kart that axle is at — **put the mass on the paired axle and you are stable,
wherever the pair sits**. So flipping the layout is not forbidden by physics; it just moves
the target the bench has to hit.

As built: `x_cg` = 455 mm of a 1165 mm wheelbase → `w_eff` 254 mm → **a_tip 0.53 g**.

## The straight flip is not an option

Swap the wheels end for end and move nothing else. The 8 kg of motors and gearboxes travel
with the driven wheels, which pulls the loaded CG 95 mm toward the new front axle — and
leaves it **615 mm** from it, i.e. further from the paired axle than it has ever been on the
current chassis:

| | x_cg | a_tip | one child sitting off-centre |
|---|---:|---:|---|
| kart as built | 455 mm | **0.53 g** | planted, margin 0.86 m/s² |
| driven pair moved to the front, bench untouched | 615 mm | **0.41 g** | **TIPS** (−0.31 m/s²) |

That is the 2026-08 mistake in mirror image: the mass ends up at the wrong end again. The
reference manoeuvre still survives it (margin 1.56), which is exactly what makes it dangerous
— it is the **asymmetric** load, one child sitting on one side, that puts a wheel in the air.

## How far the bench has to move

Every millimetre of bench travel moves the loaded CG by 66/98 = **0.673 mm** (two children out
of 98 kg all-up). Running the real controller over the real manoeuvres of
[`scenarios.hpp`](../firmware/test_host/sim/scenarios.hpp) at each position:

| bench moved forward | x_cg | a_tip | full-speed turn | limiter OFF (counter-test) | child off-centre | adult + child |
|---|---:|---:|---|---|---|---|
| — (straight flip) | 615 mm | 0.41 g | 1.56 planted | −1.57 **TIPPED** | −0.31 **TIPPED** | 0.12 planted |
| 150 mm (6″) | 514 mm | 0.48 g | 2.30 planted | −1.00 **TIPPED** | 0.41 planted | 0.77 planted |
| **237 mm (9.3″)** | 455 mm | **0.53 g** | 2.72 planted | −0.60 **TIPPED** | 0.86 planted | 1.14 planted |
| 300 mm (12″) | 413 mm | 0.56 g | 3.03 planted | −0.30 **TIPPED** | 1.18 planted | 1.41 planted |
| 386 mm (15″) | 355 mm | 0.60 g | 3.45 planted | 0.12 planted | 1.62 planted | 1.78 planted |

*(margins in m/s²; negative = a wheel came up. The limiter-off column is a counter-test — the
kart **as built** fails it too, at −0.60: it measures what the software turn limiter is
carrying, not what the geometry can do.)*

- **237 mm (9.5″) is the break-even point**: it reproduces the built kart exactly — same
  `x_cg`, same 0.53 g, same margins, same 61/39 load split. Anything less is a downgrade.
- **386 mm (15″)** is where the geometry alone keeps the kart upright with the limiter
  switched off — the safety net the current build lost on 2026-08-10.

## Why 24 cm is the hard part

Relative to the new front axle, the bench sits **1013 mm** back today and would have to sit
**776 mm** back. The seatback-to-footrest distance is 570 mm, so the children's feet land
**206 mm behind the front axle** — which is precisely where the motors, the printed gearboxes,
the #35 chains and their guards have to live, because the drivetrain follows the driven axle.

That conflict is the real finding, and it is structural rather than incidental: stability wants
the mass **on** the paired axle, and with a front-drive layout the paired axle is also the
drivetrain. Today the two requirements point in opposite directions and both are satisfied —
the kids sit near the rear axle with their feet toward an empty nose. Flipped, they compete for
the same 200 mm of deck. Options, none of them free:

1. **Lengthen the deck forward** and carry the driven axle on a beam ahead of it (the way the
   caster is carried today) — a cantilevered drivetrain at the very front, in the impact zone.
2. **Accept 150 mm of bench travel** instead of 237 → 0.48 g, below today's margin, with the
   off-centre child at 0.41 m/s² of margin instead of 0.86. Not recommended for a child.
3. **Spend the height instead of the length** — see below.

## The cheaper lever: 50 mm off the seat

`a_tip ∝ 1/h_cg`, and the loaded CG sits at 482 mm. Dropping the seat 50 mm at the
break-even bench position:

| | x_cg | a_tip | child off-centre | adult + child |
|---|---:|---:|---|---|
| bench +237 mm | 455 mm | 0.53 g | 0.86 | 1.14 |
| bench +237 mm, **seat 50 mm lower** | 455 mm | **0.59 g** | 1.24 | 1.55 |
| bench +386 mm (for comparison) | 355 mm | 0.60 g | 1.62 | 1.78 |

**50 mm of seat height buys as much as 150 mm of bench travel**, and it does not fight the
drivetrain for deck space. If any of this is ever built, that is the first place to look —
and it applies to the kart as it stands today, without flipping anything.

## Do not move the battery aft

It is tempting to follow the caster with the battery, to keep the single wheel planted. It
costs almost as much as the bench move earns: 5 kg over a 1165 mm wheelbase pushes the CG
59 mm back, i.e. **~90 mm of bench travel thrown away** (0.53 g → 0.48 g). It is also
unnecessary — at the break-even position the rear caster already carries 39 % of the weight,
exactly what the front caster carries today.

## What this study does NOT say

- The rollover criterion is **symmetric**: it depends on the CG's distance to the paired axle
  and nothing else, so the simulator literally cannot tell front-drive from rear-drive. Every
  number above is geometry.
- **Weight transfer is not modelled**, and it is the one place where the two layouts genuinely
  differ. Driven wheels at the rear (as built) gain grip when accelerating and lose it when
  braking; at the front it is the other way round. On a kart whose only brake is its motors,
  putting the drive where braking loads it is an argument *for* the flip — the counterweight
  to everything above.
- Nothing here touches the turn limiter, which is what actually keeps the kart upright in
  both layouts (`turn_limit_en`, `turn_alat_vmax` capped at 0.2).

## Reproducing the numbers

```
cd firmware/test_host
g++ -std=c++17 -O2 -I ../main -I sim study_front_drive.cpp ../main/controller_core.cpp \
    ../main/config_params.cpp -o /tmp/study && /tmp/study
```

[`study_front_drive.cpp`](../firmware/test_host/study_front_drive.cpp) is a one-off harness
(not built by CI): it feeds each candidate geometry to the same `runScenario()` the test suite
uses, so the controller, the mixing, the limiter and the physics are the shipped ones.
