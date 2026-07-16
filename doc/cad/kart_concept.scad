// kart_concept.scad — CONCEPT visuel du kart tricycle différentiel (proportions du README).
// 2 roues avant 10" motorisées (boîte 1:12,5 + poulies) · roulette arrière 10" folle ·
// banquette 2 enfants (~80 cm) · bois standard : 2×3 (38×64 mm) + contreplaqué 1/2" (12,7 mm).
// Repère : x=0 essieu avant (+x vers l'arrière), z=0 sol.

// ── Cotes principales (mm) ──
WHEEL_D = 254;   WHEEL_W = 70;      // roues 10"
TRACK   = 840;                       // voie avant (centres)
WBASE   = 1100;                      // essieu avant → roulette
CLEAR   = 80;                        // garde au sol
LU_W = 38; LU_H = 64;                // 2×3 sur chant
PLY  = 12.7;                         // contreplaqué 1/2"
SEAT_W = 800;                        // banquette 2 enfants

FLOOR_Z = CLEAR + LU_H;              // dessus des longerons = dessous du plancher
AXLE_Z  = WHEEL_D / 2;

$fn = 48;

module lumber(l) cube([l, LU_W, LU_H]);                       // 2×3 sur chant, le long de x
module lumber_y(l) cube([LU_W, l, LU_H]);                     // le long de y
module ply(x, y) cube([x, y, PLY]);

module wheel() rotate([90, 0, 0]) cylinder(d = WHEEL_D, h = WHEEL_W, center = true);
module rim()   rotate([90, 0, 0]) cylinder(d = WHEEL_D * 0.55, h = WHEEL_W + 2, center = true);

// ───────────────────────────── Châssis bois ─────────────────────────────
module frame()
{
    color("BurlyWood")
    {
        // traverse avant LARGE (porte l'essieu, au plus près des roues) + fermeture de baie
        translate([  20, -410, CLEAR]) lumber_y(820);
        translate([ 300, -410, CLEAR]) lumber_y(820);
        // longerons de l'habitacle (caisse plus étroite que la voie)
        for (sy = [-1, 1]) translate([20, sy * 300 - LU_W / 2, CLEAR]) lumber(1010);
        // traverses intermédiaire + arrière
        translate([ 640, -300, CLEAR]) lumber_y(600);
        translate([ 990, -300, CLEAR]) lumber_y(600);
        // supports d'essieu (paliers près des moyeux, ≤ 5 cm)
        for (sy = [-1, 1]) translate([-20, sy * 370 - 20, CLEAR]) cube([40, 40, AXLE_Z - CLEAR + 20]);
    }
    // plancher contreplaqué 1/2"
    color("Wheat") translate([20, -410, FLOOR_Z]) ply(310, 820);                   // baie technique
    color("Wheat") translate([330, -300, FLOOR_Z]) ply(700, 600);                  // habitacle
    // essieu avant : tige filetée 1/2" traversante
    color("Silver") translate([0, 0, AXLE_Z]) rotate([90, 0, 0]) cylinder(d = 12.7, h = 940, center = true);
}

// ───────────────────────────── Banquette 2 places ─────────────────────────────
module seat()
{
    // assise basse (CP 1/2" sur cales) + dossier incliné — banquette CONTINUE (pas de séparation)
    color("Peru")
    {
        translate([650, -SEAT_W / 2, FLOOR_Z + PLY + 30]) ply(300, SEAT_W);            // assise
        translate([950, -SEAT_W / 2, FLOOR_Z + PLY]) rotate([0, -82, 0]) ply(340, SEAT_W);  // dossier
    }
    color("BurlyWood") for (sy = [-1, 0, 1])                                            // cales d'assise
        translate([660, sy * (SEAT_W / 2 - 40) - 20, FLOOR_Z + PLY]) cube([280, 40, 30]);

    // GARDE-CORPS latéraux (CP 1/2") : empêchent l'enfant de tomber sur les côtés
    color("Peru") for (sy = [-1, 1])
        translate([580, sy * (SEAT_W / 2) - (sy > 0 ? PLY : 0), FLOOR_Z + PLY])
            cube([400, PLY, 300]);

    // Arrêt d'urgence : champignon AU SOMMET DU DOSSIER, centré (accessible aux 2 enfants
    // et à un adulte derrière le kart)
    top_x = 950 + 340 * cos(82); top_z = FLOOR_Z + PLY + 340 * sin(82);
    color("Peru")   translate([top_x - 45, -50, top_z - 6]) cube([90, 100, 14]);   // platine
    color("Yellow") translate([top_x, 0, top_z + 8]) cylinder(d = 60, h = 6);
    color("Red")    translate([top_x, 0, top_z + 12]) cylinder(d = 45, h = 26);
}

// ───────────────────────────── Propulsion (par côté) ─────────────────────────────
module drive_side(sy)
{
    // roue motrice 10"
    color("DimGray")  translate([0, sy * TRACK / 2, AXLE_Z]) wheel();
    color("Gainsboro") translate([0, sy * TRACK / 2, AXLE_Z]) rim();
    // poulie de roue (côté intérieur) + boîte (carter imprimé) + moteur
    color("Orange")   translate([0, sy * (TRACK / 2 - WHEEL_W / 2 - 12), AXLE_Z])
        rotate([90, 0, 0]) cylinder(d = 86, h = 16, center = true);
    color("SlateGray") translate([-60, sy * 330 - 20, CLEAR + 20]) cube([180, 40, 170]);   // boîte
    color("DarkGray")  translate([115, sy * 330, CLEAR + 150]) rotate([0, 90, 0]) cylinder(d = 42, h = 80);  // moteur
    color("DarkSlateGray") translate([-40, sy * (TRACK / 2 - WHEEL_W / 2 - 12), AXLE_Z - 4])   // courroie (stylisée)
        cube([40, 6, 8]);
}

// ───────────────────────────── Roulette arrière 10" (pivot BIEN VISIBLE) ─────────────────────────────
CASTER_X = 1090;            // axe de pivot (vertical)
CASTER_YAW = 38;            // fourche dessinée ORIENTÉE pour montrer que ça tourne
module caster()
{
    plat_z = FLOOR_Z + PLY + 175;                       // sous-face de la plateforme de pivot
    // support OUVERT : 2 montants 2×3 + petite plateforme (roue dégagée, visible)
    color("BurlyWood") for (sy = [-1, 1])
        translate([CASTER_X - 60, sy * 85 - LU_W / 2, FLOOR_Z + PLY]) cube([38, LU_W, 175]);
    color("Wheat") translate([CASTER_X - 80, -110, plat_z]) ply(160, 220);

    // pivot vertical apparent + fourche ORIENTÉE (chasse vers l'arrière du pivot)
    color("Silver") translate([CASTER_X, 0, plat_z - 10]) cylinder(d = 64, h = 10);   // platine
    color("Silver") translate([CASTER_X, 0, plat_z - 26]) cylinder(d = 24, h = 18);   // axe de pivot
    translate([CASTER_X, 0, 0]) rotate([0, 0, CASTER_YAW])
    {
        color("DimGray") for (sy = [-1, 1])                                           // bras de fourche
            translate([-10, sy * 42 - 4, AXLE_Z - 20]) cube([85, 8, plat_z - 26 - AXLE_Z + 20]);
        color("DimGray")  translate([55, 0, AXLE_Z]) wheel();                          // roue déportée (chasse)
        color("Gainsboro") translate([55, 0, AXLE_Z]) rim();
    }
}

// ───────────────────────────── Assemblage ─────────────────────────────
frame();
seat();
drive_side(-1);
drive_side(1);
caster();
