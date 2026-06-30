// ui_dash.c — Tableau de bord véhicule (maquette LVGL, valeurs animées en démo).
// Compteur de vitesse à aiguille (= moyenne des 2 roues) + puissance moteur G/D + barre d'état.
#include <math.h>
#include <time.h>

#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "ui_dash.h"

#define COL_BG 0x0c0e12
#define COL_CARD 0x161a22
#define COL_CYAN 0x33ccff
#define COL_GREEN 0x36e07a
#define COL_AMBER 0xffaa33
#define COL_RED 0xff5544
#define COL_GREY 0x8a93a6
#define SPD_MAX 25

// Widgets animés
static lv_obj_t* s_scale;
static lv_obj_t* s_needle;
static lv_obj_t* s_spdnum;
static lv_obj_t* s_clk;
static lv_obj_t* s_pg_val, *s_pg_bar, *s_pd_val, *s_pd_bar;
// Pavé joystick (avance/virage) : carré + croix centrale + segment centre→position + point.
static lv_obj_t* s_joy_dot, *s_joy_line;
static lv_point_precise_t s_joy_pts[2];
#define JOY 150              // côté du carré (px)
#define JOY_C (JOY / 2)      // centre (coord locales)
#define JOY_R (JOY_C - 16)   // rayon utile pour la position
// Sélecteur de mode PARK / DRIVE / BRAKE
static lv_obj_t* s_gear[3];
static const uint32_t GEAR_COL[3] = {COL_AMBER, COL_GREEN, COL_RED};
static const char* const GEAR_TXT[3] = {"PARK", "DRIVE", "BRAKE"};
static float s_phase = 0.f;

// Surligne le mode actif (chip plein + texte sombre) ; les autres restent ternes.
static void set_gear(int active)
{
    for (int i = 0; i < 3; ++i)
    {
        const bool on = (i == active);
        lv_obj_set_style_bg_color(s_gear[i], lv_color_hex(on ? GEAR_COL[i] : 0x2a2f3a), 0);
        lv_obj_set_style_text_color(s_gear[i], lv_color_hex(on ? COL_BG : COL_GREY), 0);
    }
}

// ───────────────────────── helpers ─────────────────────────
static lv_obj_t* card(lv_obj_t* parent, int w, int h)
{
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

static lv_obj_t* lbl(lv_obj_t* parent, const char* txt, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

// Tuile « libellé + valeur + barre ». Si bar_min < 0 → barre symétrique (centre = 0).
static void gauge_tile(lv_obj_t* parent, lv_align_t align, int ox, int oy, const char* title,
                       uint32_t accent, int bar_min, int bar_max, lv_obj_t** out_val, lv_obj_t** out_bar)
{
    lv_obj_t* c = card(parent, 420, 92);
    lv_obj_align(c, align, ox, oy);

    lv_obj_t* t = lbl(c, title, &lv_font_montserrat_14, COL_GREY);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t* v = lbl(c, "--", &lv_font_montserrat_28, 0xffffff);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -16, 8);

    lv_obj_t* b = lv_bar_create(c);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 388, 10);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(b, 5, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2a2f3a), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(b, bar_min, bar_max);
    if (bar_min < 0) lv_bar_set_mode(b, LV_BAR_MODE_SYMMETRICAL);   // remplit depuis le centre
    lv_bar_set_value(b, 0, LV_ANIM_OFF);

    *out_val = v;
    *out_bar = b;
}

// ───────────────────────── timers ─────────────────────────
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Animation de démonstration (cadence réglée par le timer).
static void anim_cb(lv_timer_t* t)
{
    (void)t;
    s_phase += 0.10f;
    const float av = sinf(s_phase);                  // avance  -1..1 (négatif = recul)
    const float vir = 0.8f * sinf(s_phase * 0.6f);   // virage -1..1 (négatif = gauche)

    const float cg = clampf(av - vir, -1.f, 1.f), cd = clampf(av + vir, -1.f, 1.f);
    const float spd = (fabsf(cg) + fabsf(cd)) / 2.f * SPD_MAX;   // vitesse = moyenne des 2 roues

    const int sp10 = (int)(spd * 10 + 0.5f);
    lv_scale_set_line_needle_value(s_scale, s_needle, 104, (sp10 + 5) / 10);
    lv_label_set_text_fmt(s_spdnum, "%d.%d", sp10 / 10, sp10 % 10);

    const int pg = (int)(cg * 100), pd = (int)(cd * 100);
    lv_label_set_text_fmt(s_pg_val, "%d%%", pg);
    lv_bar_set_value(s_pg_bar, pg, LV_ANIM_OFF);
    lv_label_set_text_fmt(s_pd_val, "%d%%", pd);
    lv_bar_set_value(s_pd_bar, pd, LV_ANIM_OFF);

    const int jx = (int)(vir * JOY_R), jy = (int)(-av * JOY_R);
    lv_obj_align(s_joy_dot, LV_ALIGN_CENTER, jx, jy);
    s_joy_pts[0].x = JOY_C; s_joy_pts[0].y = JOY_C;
    s_joy_pts[1].x = JOY_C + jx; s_joy_pts[1].y = JOY_C + jy;
    lv_line_set_points(s_joy_line, s_joy_pts, 2);

    static int last_gear = -1;
    const int gear = (fabsf(av) < 0.08f) ? 0 : (av > 0.f ? 1 : 2);
    if (gear != last_gear) { set_gear(gear); last_gear = gear; }
}

static void clock_cb(lv_timer_t* t)
{
    (void)t;
    const time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year > (2020 - 1900))
        lv_label_set_text_fmt(s_clk, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    else
        lv_label_set_text(s_clk, "--:--:--");
}

// ───────────────────────── construction ─────────────────────────
void ui_dash_build(void)
{
    lvgl_port_lock(0);
    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── Barre d'état (haut) ──
    lv_obj_t* top = lv_obj_create(scr);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, 800, 50);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);

    lv_obj_t* armed = lbl(top, "\xE2\x97\x8F  ARM\xC3\x89", &lv_font_montserrat_20, COL_GREEN);
    lv_obj_align(armed, LV_ALIGN_LEFT_MID, 16, 0);

    s_clk = lbl(top, "--:--:--", &lv_font_montserrat_28, 0xffffff);
    lv_obj_align(s_clk, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t* batt = lbl(top, "\xF0\x9F\x94\x8B 88%   20.4 V", &lv_font_montserrat_20, COL_GREEN);
    lv_obj_align(batt, LV_ALIGN_RIGHT_MID, -16, 0);

    // ── Compteur de vitesse (aiguille) — moyenne des 2 roues ──
    s_scale = lv_scale_create(scr);
    lv_obj_set_size(s_scale, 280, 280);
    lv_obj_align(s_scale, LV_ALIGN_LEFT_MID, 50, 12);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(s_scale, 26);
    lv_scale_set_major_tick_every(s_scale, 5);
    lv_scale_set_range(s_scale, 0, SPD_MAX);
    lv_scale_set_angle_range(s_scale, 270);
    lv_scale_set_rotation(s_scale, 135);
    lv_obj_set_style_arc_color(s_scale, lv_color_hex(0x2a2f3a), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_scale, 4, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_scale, lv_color_hex(COL_GREY), LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_scale, lv_color_hex(COL_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_scale, lv_color_hex(COL_GREY), LV_PART_INDICATOR);
    lv_obj_set_style_text_font(s_scale, &lv_font_montserrat_14, LV_PART_INDICATOR);

    s_needle = lv_line_create(s_scale);
    lv_obj_set_style_line_width(s_needle, 4, 0);
    lv_obj_set_style_line_color(s_needle, lv_color_hex(COL_RED), 0);
    lv_obj_set_style_line_rounded(s_needle, true, 0);
    lv_scale_set_line_needle_value(s_scale, s_needle, 104, 0);

    s_spdnum = lbl(scr, "0.0", &lv_font_montserrat_48, 0xffffff);
    lv_obj_set_width(s_spdnum, 200);                                  // largeur fixe…
    lv_obj_set_style_text_align(s_spdnum, LV_TEXT_ALIGN_CENTER, 0);   // …+ texte centré → reste centré
    lv_obj_align_to(s_spdnum, s_scale, LV_ALIGN_CENTER, 0, -6);
    lv_obj_t* kmh = lbl(scr, "km/h", &lv_font_montserrat_20, COL_GREY);
    lv_obj_set_width(kmh, 200);
    lv_obj_set_style_text_align(kmh, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(kmh, s_scale, LV_ALIGN_CENTER, 0, 40);

    // ── Puissance moteur G / D (haut-droite, barres symétriques -100..+100) ──
    gauge_tile(scr, LV_ALIGN_TOP_RIGHT, -18, 60, "PUISSANCE MOTEUR GAUCHE", COL_AMBER, -100, 100, &s_pg_val, &s_pg_bar);
    gauge_tile(scr, LV_ALIGN_TOP_RIGHT, -18, 156, "PUISSANCE MOTEUR DROITE", COL_AMBER, -100, 100, &s_pd_val, &s_pd_bar);

    // ── Pavé joystick : carré + croix centrale + point de position (bas-droite) ──
    lv_obj_t* jt = lbl(scr, "AVANCE / VIRAGE", &lv_font_montserrat_14, COL_GREY);
    lv_obj_align(jt, LV_ALIGN_BOTTOM_RIGHT, -90, -JOY - 26);

    lv_obj_t* pad = card(scr, JOY, JOY);
    lv_obj_align(pad, LV_ALIGN_BOTTOM_RIGHT, -150, -16);
    lv_obj_set_style_border_color(pad, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_border_width(pad, 2, 0);
    lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);

    // Croix centrale (axes fixes).
    lv_obj_t* vx = lv_obj_create(pad);
    lv_obj_remove_style_all(vx);
    lv_obj_set_size(vx, 2, 2 * JOY_R);
    lv_obj_center(vx);
    lv_obj_set_style_bg_color(vx, lv_color_hex(0x2a2f3a), 0);
    lv_obj_set_style_bg_opa(vx, LV_OPA_COVER, 0);
    lv_obj_t* hx = lv_obj_create(pad);
    lv_obj_remove_style_all(hx);
    lv_obj_set_size(hx, 2 * JOY_R, 2);
    lv_obj_center(hx);
    lv_obj_set_style_bg_color(hx, lv_color_hex(0x2a2f3a), 0);
    lv_obj_set_style_bg_opa(hx, LV_OPA_COVER, 0);

    // Segment centre → position + point.
    s_joy_line = lv_line_create(pad);
    lv_obj_set_style_line_width(s_joy_line, 3, 0);
    lv_obj_set_style_line_color(s_joy_line, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_line_rounded(s_joy_line, true, 0);
    s_joy_dot = lv_obj_create(pad);
    lv_obj_remove_style_all(s_joy_dot);
    lv_obj_set_size(s_joy_dot, 16, 16);
    lv_obj_center(s_joy_dot);
    lv_obj_set_style_radius(s_joy_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_joy_dot, lv_color_hex(COL_CYAN), 0);
    lv_obj_set_style_bg_opa(s_joy_dot, LV_OPA_COVER, 0);

    // ── Sélecteur de mode PARK / DRIVE / BRAKE (bas-gauche, pills stylisées) ──
    lv_obj_t* gear = lv_obj_create(scr);
    lv_obj_remove_style_all(gear);
    lv_obj_set_size(gear, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(gear, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_flex_flow(gear, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(gear, 12, 0);
    for (int i = 0; i < 3; ++i)
    {
        lv_obj_t* chip = lv_label_create(gear);
        lv_label_set_text(chip, GEAR_TXT[i]);
        lv_obj_set_style_text_font(chip, &lv_font_montserrat_20, 0);
        lv_obj_set_style_radius(chip, 10, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_left(chip, 18, 0);
        lv_obj_set_style_pad_right(chip, 18, 0);
        lv_obj_set_style_pad_top(chip, 10, 0);
        lv_obj_set_style_pad_bottom(chip, 10, 0);
        s_gear[i] = chip;
    }
    set_gear(0);

    lv_timer_create(anim_cb, 200, NULL);    // animation démo (5 Hz)
    lv_timer_create(clock_cb, 1000, NULL);  // horloge (1 Hz)
    lvgl_port_unlock();
}
