// gearbox.scad — Réducteur 1:8 du kart, PARAMÉTRIQUE (OpenSCAD, autonome — générateur de
// denture en développante inclus). Cotes = doc/reducteur.md : denture 24 DP / 20°,
// 16T (moteur, métal) → 64T ép. 10, puis 32T ép. 20 (pignon composé) → 64T sortie ép. 20.
// Entraxes 42,33 / 50,80 mm. Roulements 6805 (25×37×7), tourillons Ø24,9, pilote Ø10,
// 3 vis M4 sur Ø22. Carter : plaque arrière (moteur) + plaque avant dévissable.
//
// Usage : régler `part` puis F6/exporter le STL.
//   part = "assembly" (vue), "compound" (64T+32T), "output" (64T sortie),
//          "back" / "front" (plaques), "pinion_test" (pignon d'essai 16T)
// `explode` écarte les pièces dans la vue assemblage.

part = "assembly";
explode = 0;            // mm d'écartement vertical entre étages (vue assemblage)

// ───────────────────────── Paramètres denture ─────────────────────────
DP   = 24;              // diametral pitch (impérial)
PA   = 20;              // angle de pression (deg)
BKL  = 0.10;            // backlash linéaire (mm) — jeu d'impression
mmod = 25.4 / DP;       // module équivalent (1,058 mm)

// ───────────────────────── Cotes mécaniques (mm) ─────────────────────────
T64_1   = 10;           // épaisseur 64T étage 1
T32     = 20;           // épaisseur 32T
T64_OUT = 20;           // épaisseur 64T de sortie
GAP     = 1.5;          // jeu axial 64T sortie ↔ 64T étage 1

JRN_D   = 24.9;         // Ø tourillon (serrage léger dans la bague int. 6805)
BRG_D   = 37.1;         // Ø logement roulement (6805 = 37,0 + ajustement imprimé)
BRG_T   = 7;            // épaisseur roulement 6805
PILOT_D = 10;           // Ø pilote central (alignement flasques)
BC_D    = 22;           // Ø cercle des 3 vis
SCR_D   = 4.2;          // Ø passage vis M4

PLATE_T = 8;            // épaisseur plaques carter
CLR     = 1.0;          // jeu axial pièce ↔ plaque

E1 = (16 + 64) / (2 * DP) * 25.4;   // 42,33 — entraxe 16T→64T
E2 = (32 + 64) / (2 * DP) * 25.4;   // 50,80 — entraxe 32T→64T

$fn = 90;

// ───────────────────── Générateur d'engrenage (développante) ─────────────────────
function _rp(z)  = mmod * z / 2;                 // rayon primitif
function _rb(z)  = _rp(z) * cos(PA);             // rayon de base
function _ra(z)  = _rp(z) + mmod;                // rayon de tête
function _rr(z)  = _rp(z) - 1.25 * mmod;         // rayon de pied
function _roll(z, r) = let(rb = _rb(z)) sqrt(max(0, (r / rb) * (r / rb) - 1));  // angle de roulement (rad)
function _inv(u) = u * 180 / PI - atan(u);       // fonction involute (deg), u en rad
// demi-épaisseur angulaire de dent au rayon r (deg), backlash déduit
function _half(z, r) = 90 / z - (BKL / 2) / _rp(z) * 180 / PI + _inv(_roll(z, _rp(z))) - _inv(_roll(z, r));
function _pol(r, a) = [r * cos(a), r * sin(a)];

module gear2d(z)
{
    rr = _rr(z); ra = _ra(z); rb = _rb(z);
    r0 = max(rr, rb);            // départ du profil en développante
    N  = 14;                     // points par flanc
    flank = [for (i = [0:N]) let(r = r0 + (ra - r0) * i / N) _pol(r, -_half(z, r))];
    tooth = concat(
        (rr < rb) ? [_pol(rr, -_half(z, rb))] : [],          // raccord radial sous le cercle de base
        flank,
        [for (i = [0:N]) let(r = ra - (ra - r0) * i / N) _pol(r, _half(z, r))],
        (rr < rb) ? [_pol(rr, _half(z, rb))] : []
    );
    union()
    {
        circle(r = rr);
        for (k = [0:z-1]) rotate(k * 360 / z) polygon(tooth);
    }
}

module gear(z, th) { linear_extrude(height = th) gear2d(z); }

// ───────────────────── Perçages communs (pilote + 3 vis) ─────────────────────
module axial_holes(h)
{
    cylinder(d = PILOT_D, h = h);                                  // pilote central
    for (k = [0:2]) rotate(k * 120) translate([BC_D / 2, 0, 0])
        cylinder(d = SCR_D, h = h);                                // 3 vis M4
}

// ───────────────────── Pièces ─────────────────────
// Pignon composé : tourillon AR + 64T(10) + 32T(20) + tourillon AV, percé pilote + 3 vis.
module compound_gear()
{
    jl_b = BRG_T + CLR;                                   // tourillon arrière (dans le roulement AR)
    jl_f = GAP + T64_OUT - T32 + CLR + BRG_T;             // tourillon avant (atteint le fond du logement)
    difference()
    {
        union()
        {
            translate([0, 0, -jl_b]) cylinder(d = JRN_D, h = jl_b);
            gear(64, T64_1);
            translate([0, 0, T64_1]) gear(32, T32);
            translate([0, 0, T64_1 + T32]) cylinder(d = JRN_D, h = jl_f);
        }
        translate([0, 0, -jl_b - 1]) axial_holes(jl_b + T64_1 + T32 + jl_f + 2);
    }
}

// 64T de sortie : long tourillon AR (traverse le plan de l'étage 1) + tourillon AV
// prolongé au travers de la plaque avant (bout de sortie → poulie).
module output_gear()
{
    z0   = T64_1 + GAP;                        // plan inférieur de la roue (au niveau de la 32T)
    jl_b = BRG_T + CLR + z0;                   // jusqu'au roulement AR
    jl_f = BRG_T + CLR + PLATE_T + 12;         // traverse la plaque avant + 12 mm pour la poulie
    difference()
    {
        union()
        {
            translate([0, 0, -BRG_T - CLR]) cylinder(d = JRN_D, h = jl_b);
            translate([0, 0, z0]) gear(64, T64_OUT);
            translate([0, 0, z0 + T64_OUT]) cylinder(d = JRN_D, h = jl_f);
        }
        translate([0, 0, -BRG_T - CLR - 1]) axial_holes(jl_b + T64_OUT + jl_f + 2);
    }
}

// Pignon d'essai 16T (valide le standard 24 DP contre le pignon moteur).
module pinion_test() { difference() { gear(16, 8); translate([0,0,-1]) cylinder(d = 5, h = 10); } }

// ───────────────────── Carter ─────────────────────
PL_W = 162; PL_H = 92;                        // encombrement plaques
function plate_off() = [-30, 0];              // origine moteur → coin des plaques (centrage)

module plate_blank()
{
    translate([plate_off()[0], -PL_H / 2, 0])
        linear_extrude(height = PLATE_T)
            offset(r = 6) offset(delta = -6) square([PL_W, PL_H]);
}

module bearing_pocket() { translate([0, 0, PLATE_T - BRG_T]) cylinder(d = BRG_D, h = BRG_T + 1); }
module corner_holes()
{
    for (p = [[8, 8], [PL_W - 8, 8], [8, PL_H - 8], [PL_W - 8, PL_H - 8]])
        translate([plate_off()[0] + p[0], -PL_H / 2 + p[1], -1]) cylinder(d = SCR_D, h = PLATE_T + 2);
    // 2 pions de centrage Ø4 (diagonale) — répétabilité de l'entraxe
    for (p = [[20, PL_H - 8], [PL_W - 20, 8]])
        translate([plate_off()[0] + p[0], -PL_H / 2 + p[1], -1]) cylinder(d = 4.05, h = PLATE_T + 2);
}

// Plaque arrière : passage pignon moteur + 2 logements de roulements (face intérieure = z=PLATE_T).
module plate_back()
{
    difference()
    {
        plate_blank();
        translate([0, 0, -1]) cylinder(d = 22, h = PLATE_T + 2);          // passage pignon moteur
        for (a = [[0,0],[E1,0],[E1+E2,0]]) if (a[0] > 0) translate(a) bearing_pocket();
        // trous de fixation moteur (entraxe 25 mm, à adapter au moteur réel)
        for (k = [0:1]) rotate(k * 180) translate([0, 12.5, -1]) cylinder(d = 4.2, h = PLATE_T + 2);
        corner_holes();
    }
}

// Plaque avant dévissable : logements miroirs + passage du tourillon de sortie.
module plate_front()
{
    difference()
    {
        plate_blank();
        for (x = [E1, E1 + E2]) translate([x, 0, -1]) cylinder(d = BRG_D, h = BRG_T + 1);  // poches côté intérieur (z=0)
        translate([E1 + E2, 0, -1]) cylinder(d = JRN_D + 1.2, h = PLATE_T + 2);            // passage tourillon sortie
        corner_holes();
    }
}

// ───────────────────── Assemblage ─────────────────────
module assembly()
{
    color("dimgray")  translate([0, 0, -PLATE_T + 0]) plate_back();
    color("silver")   translate([0, 0, CLR]) gear(16, 6.35);                       // pignon moteur (réf.)
    color("gold")     translate([E1, 0, CLR + explode])       rotate(180 / 64) compound_gear();
    color("orange")   translate([E1 + E2, 0, CLR + 2*explode]) rotate(180 / 64 * (1 + 32 / 64)) output_gear();
    inner = CLR + T64_1 + GAP + T64_OUT + CLR;                                     // hauteur intérieure
    color("gray", 0.35) translate([0, 0, inner + 3*explode]) plate_front();
}

// ───────────────────── Sélecteur ─────────────────────
if (part == "assembly")      assembly();
else if (part == "compound") compound_gear();
else if (part == "output")   output_gear();
else if (part == "back")     plate_back();
else if (part == "front")    plate_front();
else if (part == "pinion_test") pinion_test();
