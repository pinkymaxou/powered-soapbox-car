// gearbox.scad — Kart 1:17 gearbox, PARAMETRIC (OpenSCAD, standalone — involute
// tooth generator included). Dims = doc/reducteur.md (revision 1:13.33): 24 DP / 20°,
// 16T (motor, metal) → 80T thk 10, then 30T thk 20–25 (compound pinion) → 80T output.
// Center distances 50.80 / 58.21 mm; pulleys 25T→32T (1.28:1) → total 1:17.07. Bearings 6805 (25×37×7), journals Ø24.9, pilot Ø10,
// 3× M4 screws on Ø22. Housing: rear plate (motor) + removable front plate.
//
// Usage: set `part` then F6/export the STL.
//   part = "assembly" (view), "compound" (80T+30T), "output" (80T output),
//          "back" / "front" (plates), "pinion_test" (16T test pinion)
// `explode` spreads the parts apart in the assembly view.

part = "assembly";
explode = 0;            // mm of vertical spacing (assembly view) — 0 = real stack-up

// ───────────────────────── Tooth parameters ─────────────────────────
DP   = 24;              // diametral pitch (imperial)
PA   = 20;              // pressure angle (deg)
BKL  = 0.10;            // linear backlash (mm) — print clearance
mmod = 25.4 / DP;       // equivalent module (1.058 mm)

// ───────────────────────── Mechanical dimensions (mm) ─────────────────────────
T64_1   = 10;           // stage-1 80T thickness
T32     = 20;           // 30T pinion thickness
T64_OUT = 20;           // output 80T thickness (25 recommended, see doc)
GAP     = 1.5;          // axial clearance output 80T ↔ stage-1 80T

JRN_D   = 24.9;         // journal Ø (light fit in the 6805 inner race)
BRG_D   = 37.1;         // bearing seat Ø (6805 = 37.0 + printed fit)
BRG_T   = 7;            // 6805 bearing thickness
PILOT_D = 10;           // central pilot Ø (flange alignment)
BC_D    = 22;           // Ø of the 3-screw bolt circle
SCR_D   = 4.2;          // M4 screw clearance Ø

PLATE_T = 8;            // housing plate thickness
CLR     = 1.0;          // axial clearance part ↔ plate

E1 = (16 + 80) / (2 * DP) * 25.4;   // 50.80 — center distance 16T→80T
E2 = (30 + 80) / (2 * DP) * 25.4;   // 58.21 — center distance 30T→80T

$fn = 90;

// ───────────────────── Gear generator (involute) ─────────────────────
function _rp(z)  = mmod * z / 2;                 // pitch radius
function _rb(z)  = _rp(z) * cos(PA);             // base radius
function _ra(z)  = _rp(z) + mmod;                // tip radius
function _rr(z)  = _rp(z) - 1.25 * mmod;         // root radius
function _roll(z, r) = let(rb = _rb(z)) sqrt(max(0, (r / rb) * (r / rb) - 1));  // roll angle (rad)
function _inv(u) = u * 180 / PI - atan(u);       // involute function (deg), u in rad
// angular half-thickness of a tooth at radius r (deg), backlash subtracted
function _half(z, r) = 90 / z - (BKL / 2) / _rp(z) * 180 / PI + _inv(_roll(z, _rp(z))) - _inv(_roll(z, r));
function _pol(r, a) = [r * cos(a), r * sin(a)];

module gear2d(z)
{
    rr = _rr(z); ra = _ra(z); rb = _rb(z);
    r0 = max(rr, rb);            // start of the involute profile
    ri = rr - 0.6;               // anchor BELOW the root circle: clean overlap with
                                 // the body → no coplanar faces (z-fighting in F5 preview)
    N  = 14;                     // points per flank
    flank = [for (i = [0:N]) let(r = r0 + (ra - r0) * i / N) _pol(r, -_half(z, r))];
    tooth = concat(
        [_pol(ri, -_half(z, r0))],
        flank,
        [for (i = [0:N]) let(r = ra - (ra - r0) * i / N) _pol(r, _half(z, r))],
        [_pol(ri, _half(z, r0))]
    );
    union()
    {
        circle(r = rr);
        for (k = [0:z-1]) rotate(k * 360 / z) polygon(tooth);
    }
}

module gear(z, th) { linear_extrude(height = th) gear2d(z); }

// ───────────────────── Common drillings (pilot + 3 screws) ─────────────────────
module axial_holes(h)
{
    cylinder(d = PILOT_D, h = h);                                  // central pilot
    for (k = [0:2]) rotate(k * 120) translate([BC_D / 2, 0, 0])
        cylinder(d = SCR_D, h = h);                                // 3× M4 screws
}

// ───────────────────── Parts ─────────────────────
// Compound pinion: rear journal + 80T(10) + 30T + front journal, drilled pilot + 3 screws.
module compound_gear()
{
    jl_b = BRG_T + CLR;                                   // rear journal (in the rear bearing)
    jl_f = GAP + T64_OUT - T32 + CLR + BRG_T;             // front journal (reaches the bottom of the seat)
    difference()
    {
        union()
        {
            translate([0, 0, -jl_b]) cylinder(d = JRN_D, h = jl_b);
            gear(80, T64_1);
            translate([0, 0, T64_1]) gear(30, T32);
            translate([0, 0, T64_1 + T32]) cylinder(d = JRN_D, h = jl_f);
        }
        translate([0, 0, -jl_b - 1]) axial_holes(jl_b + T64_1 + T32 + jl_f + 2);
    }
}

// output 80T: long rear journal (crosses the stage-1 plane) + front journal
// extended through the front plate (output stub → pulley).
module output_gear()
{
    z0   = T64_1 + GAP;                        // lower plane of the gear (at the 30T level)
    jl_b = BRG_T + CLR + z0;                   // up to the rear bearing
    jl_f = BRG_T + CLR + PLATE_T + 12;         // crosses the front plate + 12 mm for the pulley
    difference()
    {
        union()
        {
            translate([0, 0, -BRG_T - CLR]) cylinder(d = JRN_D, h = jl_b);
            translate([0, 0, z0]) gear(80, T64_OUT);
            translate([0, 0, z0 + T64_OUT]) cylinder(d = JRN_D, h = jl_f);
        }
        translate([0, 0, -BRG_T - CLR - 1]) axial_holes(jl_b + T64_OUT + jl_f + 2);
    }
}

// 16T test pinion (validates the 24 DP standard against the motor pinion).
module pinion_test() { difference() { gear(16, 8); translate([0,0,-1]) cylinder(d = 5, h = 10); } }

// ───────────────────── Housing (tub-style architecture) ─────────────────────
// Rear plate extended with a peripheral WALL (closed chamber) + mounting LUGS to the
// chassis at the 4 corners; cover screwed onto the wall, COUNTERBORES over the seats.
PL_W = 192; PL_H = 100;                       // footprint excluding lugs (80T Ø86.8)
WALL_T  = 4;                                  // peripheral wall thickness
EAR_W   = 22; EAR_L = 16;                     // mounting lugs (4×, at the corners)
EAR_HOLE = 5.2;                               // chassis mounting hole Ø (M5)
COVER_SCR = 4.2;                              // cover screw Ø (M4, in the wall)
INNER_H = CLR + T64_1 + GAP + T64_OUT + CLR;  // inner height of the chamber
function plate_off() = [-30, 0];              // motor origin → plate corner (centering)

module plate_outline() { offset(r = 6) offset(delta = -6) square([PL_W, PL_H]); }

// Lugs: 2 per end (corners), protrude in X, one chassis hole each.
module ears2d()
{
    for (sx = [0, 1], sy = [0, 1])
        translate([sx * PL_W + (sx ? 0 : -EAR_L), sy * (PL_H - EAR_W) ])
            offset(r = 5) offset(delta = -5) square([EAR_L + 12, EAR_W]);
}
module ear_holes()
{
    for (sx = [0, 1], sy = [0, 1])
        translate([plate_off()[0] + (sx ? PL_W + EAR_L - 9 : -EAR_L + 9),
                   -PL_H / 2 + sy * (PL_H - EAR_W) + EAR_W / 2, -1])
            cylinder(d = EAR_HOLE, h = INNER_H + 2 * PLATE_T + 2);
}
// Positions of the cover screws + dowels (along the wall axis)
function cover_scr_pos() = [for (t = [[18,0],[PL_W/2,0],[PL_W-18,0],[18,1],[PL_W/2,1],[PL_W-18,1]])
    [plate_off()[0] + t[0], -PL_H / 2 + WALL_T / 2 + t[1] * (PL_H - WALL_T)]];
function dowel_pos() = [[plate_off()[0] + WALL_T / 2 + 4, -PL_H / 2 + WALL_T / 2],
                        [plate_off()[0] + PL_W - WALL_T / 2 - 4, PL_H / 2 - WALL_T / 2]];

module bearing_pocket() { translate([0, 0, PLATE_T - BRG_T]) cylinder(d = BRG_D, h = BRG_T + 1); }

// Rear plate = floor + wall + lugs. Inner face of the floor = z = PLATE_T.
module plate_back()
{
    difference()
    {
        union()
        {
            translate([plate_off()[0], -PL_H / 2, 0])
                linear_extrude(height = PLATE_T) union() { plate_outline(); ears2d(); }
            // peripheral wall (up to the cover plane)
            translate([plate_off()[0], -PL_H / 2, PLATE_T])
                linear_extrude(height = INNER_H)
                    difference() { plate_outline(); offset(delta = -WALL_T) plate_outline(); }
        }
        translate([0, 0, -1]) cylinder(d = 22, h = PLATE_T + 2);          // motor pinion clearance hole
        for (x = [E1, E1 + E2]) translate([x, 0, 0]) bearing_pocket();    // 6805 seats (floor)
        // motor mounting holes (25 mm center distance, adapt to the actual motor)
        for (k = [0:1]) rotate(k * 180) translate([0, 12.5, -1]) cylinder(d = 4.2, h = PLATE_T + 2);
        ear_holes();
        for (q = cover_scr_pos()) translate([q[0], q[1], PLATE_T + INNER_H - 12]) cylinder(d = 3.4, h = 13);  // M4 tapping/insert
        for (q = dowel_pos())     translate([q[0], q[1], PLATE_T + INNER_H - 6])  cylinder(d = 4.05, h = 7);  // dowels
    }
}

// Cover: mirrored seats (with visible COUNTERBORE on the outer side), screws over the wall.
module plate_front()
{
    difference()
    {
        translate([plate_off()[0], -PL_H / 2, 0])
            linear_extrude(height = PLATE_T) union() { plate_outline(); ears2d(); }
        for (x = [E1, E1 + E2])
        {
            translate([x, 0, -1]) cylinder(d = BRG_D, h = BRG_T + 1);         // bearing pocket (inner)
            translate([x, 0, PLATE_T - 1.2]) cylinder(d = BRG_D + 8, h = 2);  // decorative counterbore (outer)
        }
        translate([E1 + E2, 0, -1]) cylinder(d = JRN_D + 1.2, h = PLATE_T + 2);   // output journal clearance hole
        ear_holes();
        for (q = cover_scr_pos()) translate([q[0], q[1], -1]) cylinder(d = COVER_SCR, h = PLATE_T + 2);
        for (q = dowel_pos())     translate([q[0], q[1], -1]) cylinder(d = 4.05, h = PLATE_T / 2 + 1);
    }
}

// ───────────────────── Assembly ─────────────────────
module assembly()
{
    color("dimgray")  translate([0, 0, -PLATE_T]) plate_back();
    color("silver")   translate([0, 0, CLR]) gear(16, 6.35);                       // motor pinion (ref.)
    color("gold")     translate([E1, 0, CLR + explode])       rotate(180 / 80) compound_gear();
    color("orange")   translate([E1 + E2, 0, CLR + 2*explode]) rotate(180 / 80 * (1 + 30 / 80)) output_gear();
    // TRANSLUCENT cover. ⚠️ The preview (F5/OpenCSG) sorts transparency poorly → "weird"
    // surfaces; use the FULL RENDER (F6) for a correct display.
    color("lightsteelblue", 0.30) translate([0, 0, INNER_H + 3*explode]) plate_front();
}

// ───────────────────── Selector ─────────────────────
if (part == "assembly")      assembly();
else if (part == "compound") compound_gear();
else if (part == "output")   output_gear();
else if (part == "back")     plate_back();
else if (part == "front")    plate_front();
else if (part == "pinion_test") pinion_test();
