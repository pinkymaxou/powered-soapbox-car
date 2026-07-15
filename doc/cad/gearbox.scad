// gearbox.scad — Réducteur 1:8 du kart, PARAMÉTRIQUE (OpenSCAD, autonome — générateur de
// denture en développante inclus). Cotes = doc/reducteur.md (révision 1:12,5) : 24 DP / 20°,
// 16T (moteur, métal) → 80T ép. 10, puis 32T ép. 20–25 (pignon composé) → 80T sortie.
// Entraxes 50,80 / 59,27 mm ; poulies 25T→32T (1,28:1) → total 1:16,0. Roulements 6805 (25×37×7), tourillons Ø24,9, pilote Ø10,
// 3 vis M4 sur Ø22. Carter : plaque arrière (moteur) + plaque avant dévissable.
//
// Usage : régler `part` puis F6/exporter le STL.
//   part = "assembly" (vue), "compound" (80T+32T), "output" (80T sortie),
//          "back" / "front" (plaques), "pinion_test" (pignon d'essai 16T)
// `explode` écarte les pièces dans la vue assemblage.

part = "assembly";
explode = 0;            // mm d'écartement vertical (vue assemblage) — 0 = empilement réel

// ───────────────────────── Paramètres denture ─────────────────────────
DP   = 24;              // diametral pitch (impérial)
PA   = 20;              // angle de pression (deg)
BKL  = 0.10;            // backlash linéaire (mm) — jeu d'impression
mmod = 25.4 / DP;       // module équivalent (1,058 mm)

// ───────────────────────── Cotes mécaniques (mm) ─────────────────────────
T64_1   = 10;           // épaisseur 80T étage 1
T32     = 20;           // épaisseur 32T
T64_OUT = 20;           // épaisseur 80T de sortie (25 recommandé, voir doc)
GAP     = 1.5;          // jeu axial 80T sortie ↔ 80T étage 1

JRN_D   = 24.9;         // Ø tourillon (serrage léger dans la bague int. 6805)
BRG_D   = 37.1;         // Ø logement roulement (6805 = 37,0 + ajustement imprimé)
BRG_T   = 7;            // épaisseur roulement 6805
PILOT_D = 10;           // Ø pilote central (alignement flasques)
BC_D    = 22;           // Ø cercle des 3 vis
SCR_D   = 4.2;          // Ø passage vis M4

PLATE_T = 8;            // épaisseur plaques carter
CLR     = 1.0;          // jeu axial pièce ↔ plaque

E1 = (16 + 80) / (2 * DP) * 25.4;   // 50,80 — entraxe 16T→80T
E2 = (32 + 80) / (2 * DP) * 25.4;   // 59,27 — entraxe 32T→80T

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
    ri = rr - 0.6;               // ancrage SOUS le cercle de pied : chevauchement franc avec
                                 // le corps → pas de faces coplanaires (z-fighting en aperçu F5)
    N  = 14;                     // points par flanc
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

// ───────────────────── Perçages communs (pilote + 3 vis) ─────────────────────
module axial_holes(h)
{
    cylinder(d = PILOT_D, h = h);                                  // pilote central
    for (k = [0:2]) rotate(k * 120) translate([BC_D / 2, 0, 0])
        cylinder(d = SCR_D, h = h);                                // 3 vis M4
}

// ───────────────────── Pièces ─────────────────────
// Pignon composé : tourillon AR + 80T(10) + 32T + tourillon AV, percé pilote + 3 vis.
module compound_gear()
{
    jl_b = BRG_T + CLR;                                   // tourillon arrière (dans le roulement AR)
    jl_f = GAP + T64_OUT - T32 + CLR + BRG_T;             // tourillon avant (atteint le fond du logement)
    difference()
    {
        union()
        {
            translate([0, 0, -jl_b]) cylinder(d = JRN_D, h = jl_b);
            gear(80, T64_1);
            translate([0, 0, T64_1]) gear(32, T32);
            translate([0, 0, T64_1 + T32]) cylinder(d = JRN_D, h = jl_f);
        }
        translate([0, 0, -jl_b - 1]) axial_holes(jl_b + T64_1 + T32 + jl_f + 2);
    }
}

// 80T de sortie : long tourillon AR (traverse le plan de l'étage 1) + tourillon AV
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
            translate([0, 0, z0]) gear(80, T64_OUT);
            translate([0, 0, z0 + T64_OUT]) cylinder(d = JRN_D, h = jl_f);
        }
        translate([0, 0, -BRG_T - CLR - 1]) axial_holes(jl_b + T64_OUT + jl_f + 2);
    }
}

// Pignon d'essai 16T (valide le standard 24 DP contre le pignon moteur).
module pinion_test() { difference() { gear(16, 8); translate([0,0,-1]) cylinder(d = 5, h = 10); } }

// ───────────────────── Carter (architecture « bac » façon version Fusion) ─────────────────────
// Plaque arrière prolongée d'un MURET périphérique (chambre fermée) + OREILLES de fixation
// châssis aux 4 coins ; couvercle vissé sur le muret, LAMAGES au droit des logements.
PL_W = 192; PL_H = 100;                       // encombrement hors oreilles (80T Ø86,8)
WALL_T  = 4;                                  // épaisseur du muret périphérique
EAR_W   = 22; EAR_L = 16;                     // oreilles de fixation (4×, aux coins)
EAR_HOLE = 5.2;                               // Ø trou de fixation châssis (M5)
COVER_SCR = 4.2;                              // Ø vis du couvercle (M4, dans le muret)
INNER_H = CLR + T64_1 + GAP + T64_OUT + CLR;  // hauteur intérieure de la chambre
function plate_off() = [-30, 0];              // origine moteur → coin des plaques (centrage)

module plate_outline() { offset(r = 6) offset(delta = -6) square([PL_W, PL_H]); }

// Oreilles : 2 par extrémité (coins), dépassent en X, un trou châssis chacune.
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
// Positions des vis + pions du couvercle (dans l'axe du muret)
function cover_scr_pos() = [for (t = [[18,0],[PL_W/2,0],[PL_W-18,0],[18,1],[PL_W/2,1],[PL_W-18,1]])
    [plate_off()[0] + t[0], -PL_H / 2 + WALL_T / 2 + t[1] * (PL_H - WALL_T)]];
function dowel_pos() = [[plate_off()[0] + WALL_T / 2 + 4, -PL_H / 2 + WALL_T / 2],
                        [plate_off()[0] + PL_W - WALL_T / 2 - 4, PL_H / 2 - WALL_T / 2]];

module bearing_pocket() { translate([0, 0, PLATE_T - BRG_T]) cylinder(d = BRG_D, h = BRG_T + 1); }

// Plaque arrière = fond + muret + oreilles. Face intérieure du fond = z = PLATE_T.
module plate_back()
{
    difference()
    {
        union()
        {
            translate([plate_off()[0], -PL_H / 2, 0])
                linear_extrude(height = PLATE_T) union() { plate_outline(); ears2d(); }
            // muret périphérique (jusqu'au plan du couvercle)
            translate([plate_off()[0], -PL_H / 2, PLATE_T])
                linear_extrude(height = INNER_H)
                    difference() { plate_outline(); offset(delta = -WALL_T) plate_outline(); }
        }
        translate([0, 0, -1]) cylinder(d = 22, h = PLATE_T + 2);          // passage pignon moteur
        for (x = [E1, E1 + E2]) translate([x, 0, 0]) bearing_pocket();    // logements 6805 (fond)
        // trous de fixation moteur (entraxe 25 mm, à adapter au moteur réel)
        for (k = [0:1]) rotate(k * 180) translate([0, 12.5, -1]) cylinder(d = 4.2, h = PLATE_T + 2);
        ear_holes();
        for (q = cover_scr_pos()) translate([q[0], q[1], PLATE_T + INNER_H - 12]) cylinder(d = 3.4, h = 13);  // taraudage/insert M4
        for (q = dowel_pos())     translate([q[0], q[1], PLATE_T + INNER_H - 6])  cylinder(d = 4.05, h = 7);  // pions
    }
}

// Couvercle : logements miroirs (avec LAMAGE apparent côté extérieur), vis au droit du muret.
module plate_front()
{
    difference()
    {
        translate([plate_off()[0], -PL_H / 2, 0])
            linear_extrude(height = PLATE_T) union() { plate_outline(); ears2d(); }
        for (x = [E1, E1 + E2])
        {
            translate([x, 0, -1]) cylinder(d = BRG_D, h = BRG_T + 1);         // poche roulement (int.)
            translate([x, 0, PLATE_T - 1.2]) cylinder(d = BRG_D + 8, h = 2);  // lamage décoratif ext.
        }
        translate([E1 + E2, 0, -1]) cylinder(d = JRN_D + 1.2, h = PLATE_T + 2);   // passage tourillon sortie
        ear_holes();
        for (q = cover_scr_pos()) translate([q[0], q[1], -1]) cylinder(d = COVER_SCR, h = PLATE_T + 2);
        for (q = dowel_pos())     translate([q[0], q[1], -1]) cylinder(d = 4.05, h = PLATE_T / 2 + 1);
    }
}

// ───────────────────── Assemblage ─────────────────────
module assembly()
{
    color("dimgray")  translate([0, 0, -PLATE_T]) plate_back();
    color("silver")   translate([0, 0, CLR]) gear(16, 6.35);                       // pignon moteur (réf.)
    color("gold")     translate([E1, 0, CLR + explode])       rotate(180 / 80) compound_gear();
    color("orange")   translate([E1 + E2, 0, CLR + 2*explode]) rotate(180 / 80 * (1 + 32 / 80)) output_gear();
    // Couvercle TRANSLUCIDE. ⚠️ L'aperçu (F5/OpenCSG) trie mal la transparence → surfaces
    // « bizarres » ; utiliser le RENDU COMPLET (F6) pour un affichage correct.
    color("lightsteelblue", 0.30) translate([0, 0, INNER_H + 3*explode]) plate_front();
}

// ───────────────────── Sélecteur ─────────────────────
if (part == "assembly")      assembly();
else if (part == "compound") compound_gear();
else if (part == "output")   output_gear();
else if (part == "back")     plate_back();
else if (part == "front")    plate_front();
else if (part == "pinion_test") pinion_test();
