// kart_concept.scad — CONCEPT visuel du kart : TRICYCLE INVERSÉ (proportions du README).
// 1 roulette 10" folle AVANT au centre · 2 roues 10" motorisées ARRIÈRE (boîte 1:12,5 +
// chaîne #35 25T→32T) · banquette 2 enfants 6″ DEVANT l'essieu moteur · batterie dans le
// museau, au-dessus de la roulette · bois standard : 2×3 (38×64) + contreplaqué 1/2" (12,7).
//
// ⚠️ C'est la disposition qui donne la stabilité : un tricycle bascule sur la ligne qui va
// de sa roue SEULE à l'une des roues APPAIRÉES, donc la demi-voie utile vaut
// (distance CG→roue seule)/empattement. La masse posée SUR l'essieu appairé en gardait 80 %
// (a_tip 0,69 g) ; la première version faisait l'inverse et n'atteignait que 0,39 g.
// ⚠️ 2026-08-10 : la banquette a avancé de 6″ et l'essieu a reculé de 6″ — 12″ d'écart
// entre les passagers et l'essieu sur lequel ils étaient assis. w_eff 332 → 254 mm,
// a_tip 0,69 → 0,53 g, charge sur les roues motrices 79 % → 61 %. À partir de là le
// limiteur logiciel N'EST PLUS une redondance : sans lui la simulation renverse le kart.
// Ne JAMAIS déplacer de masse vers la roulette, et ne pas éloigner davantage la banquette.
//
// Repère : x=0 = pivot de la roulette avant (+x vers l'arrière), z=0 sol.

// ── Cotes principales (mm) ──
WHEEL_D = 254;   WHEEL_W = 70;       // roues 10" (motrices ET roulette : un seul type)
TRACK   = 832;                       // voie arrière = largeur plancher + largeur de roue
PLAT_W  = 762;   PLAT_L = 1168;      // plateforme 30" × 46"
DECK_X0 = 150;   DECK_X1 = DECK_X0 + PLAT_L;   // 150 → 1318 (la roulette est 150 mm devant)
SHIFT_6IN = 6 * 25.4;                // le déplacement du 2026-08-10, 6″ de chaque côté
DRIVE_X = 1013 + SHIFT_6IN;          // 1165 — essieu moteur RECULÉ de 6″ (le plancher n'a pas bougé)
CLEAR   = 80;                        // garde au sol sous les cales
RISER   = 102;                       // cales 4" : l'essieu descend de 4" sous la structure
LU_W = 38; LU_H = 64;                // 2×3 sur chant
PLY  = 12.7;                         // contreplaqué 1/2"
SEAT_W = 762;                        // banquette 2 enfants = largeur du plancher

FLOOR_Z   = CLEAR + RISER + LU_H;    // 246 — dessus des longerons = dessous du plancher
FLOOR_TOP = FLOOR_Z + PLY;           // ~259 — surface du plancher
AXLE_Z    = WHEEL_D / 2;             // 127
RAIL_Y    = PLAT_W / 2;              // 381 — face EXTERNE des 2×3 : les roues accotent dessus
CASTER_PLATE = WHEEL_D + 78;         // 332 — sous-face de la platine de la roulette

$fn = 48;

module lumber(l)   cube([l, LU_W, LU_H]);      // 2×3 sur chant, le long de x
module lumber_y(l) cube([LU_W, l, LU_H]);      // le long de y
module ply(x, y)   cube([x, y, PLY]);

module wheel() rotate([90, 0, 0]) cylinder(d = WHEEL_D, h = WHEEL_W, center = true);
module rim()   rotate([90, 0, 0]) cylinder(d = WHEEL_D * 0.55, h = WHEEL_W + 2, center = true);

// ───────────────────────────── Châssis bois ─────────────────────────────
// Les deux 2×3 courent sur TOUTE la longueur et débordent de 150 mm vers l'avant pour
// porter la roulette. Ils sont posés au ras des bords du plancher, côté INTÉRIEUR : les
// roues motrices, glissées dans les encoches du plancher, viennent accoter dessus.
module frame()
{
    color("BurlyWood")
    {
        for (sy = [-1, 1])
            translate([0, sy * RAIL_Y - (sy > 0 ? LU_W : 0), FLOOR_Z - LU_H]) lumber(DECK_X1);
        // traverses : nez (poutre de roulette), avant de baie, milieu, essieu, arrière
        for (x = [0, DECK_X0, 560, DRIVE_X - LU_W / 2, DECK_X1 - LU_W])
            translate([x, -RAIL_Y + LU_W, FLOOR_Z - LU_H]) lumber_y(2 * (RAIL_Y - LU_W));
    }
    // plancher 30"×46" — encoches latérales pour glisser les roues motrices en place
    color("Wheat") difference()
    {
        translate([DECK_X0, -PLAT_W / 2, FLOOR_Z]) ply(PLAT_L, PLAT_W);
        for (sy = [-1, 1])
            translate([DRIVE_X - 150, sy * (PLAT_W / 2 - 55) - 30, FLOOR_Z - 1])
                cube([300, 60, PLY + 2]);
    }
}

// ── Cales d'essieu (4") : l'essieu descend de RISER sous les longerons ──
module axle_group()
{
    color("BurlyWood") for (sy = [-1, 1])
        translate([DRIVE_X - 60, sy * (RAIL_Y - 20) - 20, CLEAR]) cube([120, 40, RISER]);
    color("Silver") translate([DRIVE_X, 0, AXLE_Z]) rotate([90, 0, 0])
        cylinder(d = 12.7, h = 940, center = true);
}

// Batterie moto 12 V (~150×87×105) — DANS LE MUSEAU, au-dessus de la roulette, dans un bac
// de contention. C'est la seule masse qu'on veut à l'avant : elle plaque la roulette au sol
// (~20 % de la charge) sans coûter de marge au renversement, puisqu'elle est à l'extrémité
// opposée du triangle de basculement.
module battery()
{
    bx = DECK_X0 + 25;
    color("BurlyWood") for (sy = [-1, 1])
        translate([bx - 15, sy * 60 - (sy > 0 ? 0 : 12), FLOOR_TOP]) cube([180, 12, 70]);
    translate([bx, -44, FLOOR_TOP])
    {
        color("DimGray") cube([150, 88, 105]);
        color("Black")   translate([0, 0, 105]) cube([150, 88, 6]);
        color("Red")     translate([25, 44, 111]) cylinder(d = 14, h = 12);   // borne +
        color("Silver")  translate([125, 44, 111]) cylinder(d = 14, h = 12);  // borne −
    }
}

// ───────────────────────────── Banquette 2 places ─────────────────────────────
// La banquette a AVANCÉ de 6″ : l'assise n'est plus par-dessus l'essieu moteur mais 6″
// devant. Chaque centimètre supplémentaire vers l'avant est de la marge de renversement
// perdue — c'est déjà 78 mm de w_eff pour ces 6″-là.
module seat()
{
    back_x = DECK_X1 - 12 - SHIFT_6IN;
    color("Peru")
    {
        translate([DECK_X1 - 300 - SHIFT_6IN, -SEAT_W / 2, FLOOR_TOP + 30]) ply(300, SEAT_W);  // assise
        translate([back_x, -SEAT_W / 2, FLOOR_TOP]) rotate([0, -82, 0]) ply(340, SEAT_W); // dossier
    }
    color("BurlyWood") for (sy = [-1, 0, 1])                                             // cales d'assise
        translate([DECK_X1 - 290 - SHIFT_6IN, sy * (SEAT_W / 2 - 40) - 20, FLOOR_TOP]) cube([280, 40, 30]);

    // GARDE-CORPS latéraux (CP 1/2") : empêchent l'enfant de tomber sur les côtés
    color("Peru") for (sy = [-1, 1])
        translate([DECK_X1 - 370 - SHIFT_6IN, sy * (SEAT_W / 2) - (sy > 0 ? PLY : 0), FLOOR_TOP])
            cube([370, PLY, 300]);

    // Arrêt d'urgence : champignon AU SOMMET DU DOSSIER, centré (accessible aux 2 enfants
    // et à un adulte derrière le kart). Il ouvre la BOBINE du relais 40 A.
    top_x = back_x + 340 * cos(82); top_z = FLOOR_TOP + 340 * sin(82);
    color("Peru")   translate([top_x - 45, -50, top_z - 6]) cube([90, 100, 14]);   // platine
    color("Yellow") translate([top_x, 0, top_z + 8]) cylinder(d = 60, h = 6);
    color("Red")    translate([top_x, 0, top_z + 12]) cylinder(d = 45, h = 26);
}

// ────────────────── Propulsion arrière (par côté) : boîte 1:12,5 + chaîne #35 ──────────────────
R32 = 48.6;   // rayon primitif du pignon de roue 32T (#35, pas 9,525)
R25 = 38.0;   // rayon primitif du pignon de sortie de boîte 25T
CDIST = 165;  // entraxe choisi (doc/reducteur.md) — la boîte est DEVANT l'essieu
module drive_side(sy)
{
    color("DimGray")   translate([DRIVE_X, sy * TRACK / 2, AXLE_Z]) wheel();
    color("Gainsboro") translate([DRIVE_X, sy * TRACK / 2, AXLE_Z]) rim();
    // pignon 32T côté intérieur de la roue
    color("Orange") translate([DRIVE_X, sy * (TRACK / 2 - WHEEL_W / 2 - 12), AXLE_Z])
        rotate([90, 0, 0]) cylinder(r = R32, h = 10, center = true);
    // sortie de boîte 25T à CDIST devant, LÉGÈREMENT PLUS HAUT (le mou de la chaîne pend
    // sur le brin, pas dans les dents — voir doc/schematics/chain_layout.png)
    gx = DRIVE_X - CDIST * 0.94;  gz = AXLE_Z + CDIST * 0.34;
    color("Orange") translate([gx, sy * (TRACK / 2 - WHEEL_W / 2 - 12), gz])
        rotate([90, 0, 0]) cylinder(r = R25, h = 10, center = true);
    // brins de chaîne (stylisés)
    color("DarkSlateGray") for (dz = [R32 - 8, -R32 + 8])
        translate([gx, sy * (TRACK / 2 - WHEEL_W / 2 - 12), AXLE_Z + dz])
            rotate([0, 90 - atan2(gz - AXLE_Z, DRIVE_X - gx), 0]) cube([6, 6, CDIST], center = true);
    // carter imprimé + moteur
    color("SlateGray") translate([gx - 90, sy * (RAIL_Y - 60) - 20, gz - 85]) cube([180, 40, 170]);
    color("DarkGray")  translate([gx + 90, sy * (RAIL_Y - 60), gz]) rotate([0, 90, 0])
        cylinder(d = 42, h = 80);
}

// ───────────────────── Roulette 10" AVANT, au centre (pivot BIEN VISIBLE) ─────────────────────
CASTER_YAW = 38;            // fourche dessinée ORIENTÉE pour montrer que ça tourne
module caster()
{
    // la platine doit être à CASTER_PLATE : un petit socle sur la poutre de nez l'y amène
    color("BurlyWood") translate([-60, -110, FLOOR_Z]) cube([170, 220, CASTER_PLATE - FLOOR_Z - 10]);
    color("Silver") translate([0, 0, CASTER_PLATE - 10]) cylinder(d = 64, h = 10);   // platine
    color("Silver") translate([0, 0, CASTER_PLATE - 26]) cylinder(d = 24, h = 18);   // axe de pivot
    rotate([0, 0, CASTER_YAW])
    {
        color("DimGray") for (sy = [-1, 1])                                          // bras de fourche
            translate([-10, sy * 42 - 4, AXLE_Z - 20]) cube([85, 8, CASTER_PLATE - 26 - AXLE_Z + 20]);
        color("DimGray")  translate([55, 0, AXLE_Z]) wheel();                         // roue déportée (chasse)
        color("Gainsboro") translate([55, 0, AXLE_Z]) rim();
    }
}

// ───────────────────────────── Assemblage ─────────────────────────────
frame();
axle_group();
seat();
battery();
caster();
drive_side(-1);
drive_side(1);
