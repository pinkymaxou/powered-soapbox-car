// controller.cpp — Logique de contrôle du kart (machine à états + PID + sécurités).
// Pas de classe : un namespace anonyme tient l'état, la boucle tourne directement
// dans la tâche FreeRTOS. Le matériel est derrière `board` (hardware.cpp), l'affichage
// du ruban dans sa propre tâche (leds.cpp).
// ESP-IDF 6.1 / C++.
//
// Accélérateur (ADC, calibré) → consigne rampée → [PID option] → PWM plafonné →
// driver → 2 moteurs 12 V. Retours : encodeurs (vitesse), Vbat (LVC).
// Relâcher l'accélérateur ⇒ frein électrique. Marche arrière par bouton momentané.
// Armement par appui maintenu sur START ; désarmement auto après inactivité.
// Watchdog 5 s : si la boucle se bloque, l'ESP32 redémarre (désarmé).
#include "controller.hpp"

#include <algorithm>
#include <cmath>

#include "config.hpp"
#include "hardware.hpp"
#include "pid.hpp"
#include "rtos.hpp"

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "kart";

namespace
{
constexpr float PI_F = 3.14159265358979f;

inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// delta = Δcounts AS5600 (12 bits, 4096/tour) sur un tick. Capteur sur l'essieu → vitesse roue directe.
float pulsesToKmh(int delta)
{
    float sensor_rps = (fabsf(static_cast<float>(delta)) / hw::AS5600_CPR) * hw::CTRL_HZ;
    float wheel_rps  = sensor_rps / hw::GEAR_RATIO;
    return wheel_rps * PI_F * hw::WHEEL_DIAM_M * 3.6f;
}

// ── État interne de la boucle de contrôle ──
Pid     m_brake_pid;   // ramène la vitesse à 0 quand l'accélérateur est relâché
Pid     m_speed_pid;   // plafonne le PWM quand la vitesse dépasse la limite
State   m_state = State::Lockout;
float   m_thr_out = 0;
bool    m_lvc_tripped = false;
bool    m_rev_active = false;
int64_t m_sag_start_us = 0;
int64_t m_lvc_since_us = 0;    // instant où la LVC s'est déclenchée (→ coupure auto)
int64_t m_hold_start_us = 0;   // début d'appui START
int64_t m_last_act_us = 0;     // dernière activité accélérateur
bool    m_start_latch = false; // appui START déjà traité (attend le relâché)
int64_t m_stuck_us = 0;        // début de « PWM actif sans rotation » (encodeur unique)
bool    m_enc_fault = false;   // panne encodeur détectée (PWM actif, 0 rotation > 1 s)

void setState(State s, Fault f)
{
    m_state = s;
    g_status.m_state.store(static_cast<int>(s));
    g_status.m_fault.store(static_cast<int>(f));
}

float mapThrottle(int raw, const KartConfig& cfg)
{
    int mn = iround(cfg.thr_min_raw);
    int mx = iround(cfg.thr_max_raw);
    if (mx <= mn)
    {
        return 0;
    }
    float t = static_cast<float>(raw - mn) / static_cast<float>(mx - mn);
    t = (t - cfg.thr_deadzone) / (1.0f - cfg.thr_deadzone - cfg.thr_top_margin);
    return clampf(t, 0.f, 1.f);
}

void updateLVC(float vbat, const KartConfig& cfg)
{
    int64_t now = esp_timer_get_time();
    if (!m_lvc_tripped)
    {
        if (vbat < cfg.vbat_cut_v)
        {
            if (0 == m_sag_start_us)
            {
                m_sag_start_us = now;
            }
            if ((now - m_sag_start_us) > static_cast<int64_t>(hw::VBAT_SAG_DEBOUNCE_MS) * 1000)
            {
                if (!m_lvc_tripped) m_lvc_since_us = now;
                m_lvc_tripped = true;
            }
        }
        else
        {
            m_sag_start_us = 0;
        }
    }
    else if (vbat > cfg.vbat_recover_v)
    {
        m_lvc_tripped = false;
        m_sag_start_us = 0;
        m_lvc_since_us = 0;
    }
}

void waitMs(int ms)
{
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// Détection de panne : si on commande un moteur (|PWM| > seuil) mais que l'encodeur
// ne bouge pas (vitesse nulle) pendant > 1 s, on suppose un encodeur (ou une transmission)
// en faute. Couvre aussi un blocage moteur / une courroie cassée.
void updateEncStuck(int64_t now, float cmd, float speed_kmh, int64_t& stuck_us)
{
    if (fabsf(cmd) > hw::ENC_STUCK_PWM && speed_kmh <= 0.0f)
    {
        if (0 == stuck_us)
        {
            stuck_us = now;
        }
        else if ((now - stuck_us) > static_cast<int64_t>(hw::ENC_STUCK_MS) * 1000)
        {
            m_enc_fault = true;
        }
    }
    else
    {
        stuck_us = 0;
    }
}

int sampleAvg(int ms)
{
    long acc = 0;
    int n = 0;
    for (int k = 0, iters = ms / hw::CTRL_DT_MS; k < iters; ++k)
    {
        acc += board::throttleRaw(hw::ADC_OVERSAMPLE);
        ++n;
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(hw::CTRL_DT_MS));
    }
    return n ? static_cast<int>(acc / n) : 0;
}

// Calibration : appui à FOND (MAX) puis relâché (MIN), persistée.
void calibrate(KartConfig base)
{
    setState(State::Calibrate, Fault::None);
    board::motorsStop();
    ESP_LOGI(TAG, "CALIBRATION : appuyer à FOND sur l'accélérateur...");
    board::led(true);
    int mx = sampleAvg(2500);
    ESP_LOGI(TAG, "  MAX=%d ; relâcher...", mx);
    for (int k = 0; k < 20; ++k)
    {
        board::ledToggle();
        waitMs(100);
    }
    int mn = sampleAvg(2500);
    ESP_LOGI(TAG, "  MIN=%d", mn);
    board::led(false);

    if (mx - mn >= hw::THR_MIN_VALID_SPAN)
    {
        base.thr_min_raw = static_cast<float>(mn);
        base.thr_max_raw = static_cast<float>(mx);
        configUpdate(base, true);
        ESP_LOGI(TAG, "Calibration OK et sauvegardée");
    }
    else
    {
        ESP_LOGE(TAG, "Calibration invalide (span %d) → ignorée", mx - mn);
    }
    m_thr_out = 0;
    setState(State::Lockout, Fault::None);
}

void publish(float speed, float vbat)
{
    g_status.m_vbat.store(vbat);
    g_status.m_speed.store(speed);   // une seule vitesse (capteur unique sur l'essieu)
    g_status.m_estop.store(false);   // pas de capteur e-stop (coupure matérielle)
}

void tick()
{
    const KartConfig cfg = configSnapshot();
    const int64_t now = esp_timer_get_time();

    board::pollButtons();   // anti-rebond (une fois par tick)

    // Lectures
    bool  start_btn = board::btnStart();
    bool  rev_btn   = board::btnReverse();
    int   thr_raw   = board::throttleRaw(hw::ADC_OVERSAMPLE);
    float vbat      = board::vbatVolts(hw::ADC_OVERSAMPLE) * cfg.vbat_div_ratio;
    float spd = pulsesToKmh(board::encLeftDelta());   // AS5600 sur l'essieu (I2C) — vitesse unique

    bool  thr_fault = (thr_raw < hw::THR_FAULT_RAW_LOW) || (thr_raw > hw::THR_FAULT_RAW_HIGH);
    bool  calibrated = (iround(cfg.thr_max_raw) - iround(cfg.thr_min_raw)) >= hw::THR_MIN_VALID_SPAN;
    float thr_frac = mapThrottle(thr_raw, cfg);
    bool  at_rest = thr_frac <= hw::THR_REST_FRAC;

    updateLVC(vbat, cfg);

    // Télémétrie : entrées + sorties par défaut (écrasées si on roule)
    g_status.m_thr_raw.store(thr_raw);
    g_status.m_throttle.store(m_thr_out);
    g_status.m_btn_start.store(start_btn);
    g_status.m_btn_rev.store(rev_btn);
    g_status.m_out_l.store(0.f);
    g_status.m_out_r.store(0.f);
    g_status.m_brake.store(false);
    g_status.m_rev_led.store(false);
    g_status.m_arming.store(false);

    // Défauts (priorité absolue)
    Fault fault = Fault::None;
    if (m_lvc_tripped)
    {
        fault = Fault::Lvc;
    }
    else if (thr_fault)
    {
        fault = Fault::Throttle;
    }
    else if (!calibrated)
    {
        fault = Fault::NotCalibrated;
    }
    else if (m_enc_fault)
    {
        fault = Fault::Encoder;
    }

    if (Fault::None != fault)
    {
        m_thr_out = 0;
        board::motorsStop();
        m_rev_active = false;
        m_hold_start_us = 0;
        m_start_latch = false;
        setState(State::Fault, fault);
        publish(spd, vbat);
        // Batterie trop basse trop longtemps → l'ESP coupe son propre maintien d'alimentation.
        if (Fault::Lvc == fault && 0 != m_lvc_since_us &&
            (now - m_lvc_since_us) > static_cast<int64_t>(hw::LVC_POWEROFF_MS) * 1000)
        {
            ESP_LOGW(TAG, "Batterie trop basse > %d s → coupure de l'alimentation", hw::LVC_POWEROFF_MS / 1000);
            board::powerOff();
        }
        return;
    }

    bool armed = (State::Run == m_state);

    // Calibration : UNIQUEMENT désarmé et à l'arrêt
    bool cal_req = (1 == g_status.m_cmd.exchange(0));   // déclenchée uniquement via le web
    if (cal_req && !armed && at_rest)
    {
        calibrate(cfg);
        m_hold_start_us = 0;
        m_start_latch = false;
        m_rev_active = false;
        return;
    }

    // Bouton START maintenu = bascule ARMÉ ⇄ DÉSARMÉ (latch jusqu'au relâché)
    bool toggle = false;
    if (start_btn)
    {
        if (!m_start_latch)
        {
            if (0 == m_hold_start_us)
            {
                m_hold_start_us = now;
            }
            if ((now - m_hold_start_us) >= static_cast<int64_t>(iround(cfg.arm_hold_ms)) * 1000)
            {
                toggle = true;
                m_start_latch = true;
                m_hold_start_us = 0;
            }
        }
    }
    else
    {
        m_hold_start_us = 0;
        m_start_latch = false;
    }
    g_status.m_arming.store(!armed && 0 != m_hold_start_us);

    if (toggle)
    {
        if (armed)
        {
            armed = false;   // appui maintenu en roulant → désarmement
        }
        else if (at_rest && vbat >= cfg.vbat_recover_v)
        {
            setState(State::Run, Fault::None);
            m_last_act_us = now;
            m_thr_out = 0;
            m_stuck_us = 0;
            m_enc_fault = false;
            m_brake_pid.reset();
            m_speed_pid.reset();
            armed = true;
        }
        else
        {
            ESP_LOGW(TAG, "Armement refusé (pédale enfoncée ou batterie insuffisante : %.2f V)", vbat);
        }
    }

    // DÉSARMÉ : moteurs coupés
    if (!armed)
    {
        m_thr_out = 0;
        board::motorsStop();
        m_rev_active = false;
        setState(State::Lockout, Fault::None);
        publish(spd, vbat);
        return;
    }

    // ARMÉ : désarmement auto après inactivité de l'accélérateur
    if (thr_frac > hw::THR_REST_FRAC)
    {
        m_last_act_us = now;
    }
    if ((now - m_last_act_us) > static_cast<int64_t>(iround(cfg.disarm_s)) * 1000000)
    {
        m_thr_out = 0;
        board::motorsStop();
        m_rev_active = false;
        setState(State::Lockout, Fault::None);
        publish(spd, vbat);
        return;
    }

    m_rev_active = rev_btn && (cfg.allow_reverse != 0.f);

    // Consigne + rampe (descente rapide quand on relâche)
    float target = thr_frac;
    if (vbat < cfg.vbat_warn_v)
    {
        target *= hw::VBAT_WARN_DERATE;
    }
    float ramp = (target < m_thr_out) ? hw::THR_BRAKE_RAMP_PER_S : cfg.thr_ramp_per_s;
    m_thr_out = clampf(target, m_thr_out - ramp * hw::CTRL_DT_S, m_thr_out + ramp * hw::CTRL_DT_S);
    m_thr_out = clampf(m_thr_out, 0.f, 1.f);

    uint32_t cap = static_cast<uint32_t>(hw::PWM_MAX * clampf(cfg.duty_cap_frac, 0.f, 1.f));
    float cmd = 0;      // commande envoyée AUX DEUX moteurs (même PWM), signée [-1..1]
    bool ebrake = false;

    if (m_thr_out <= 1e-3f && !m_rev_active)
    {
        // Accélérateur relâché → freinage : le PID ramène la vitesse (encodeur) à 0.
        // Sa sortie est signée : négative ⇒ le moteur est inversé (plugging actif).
        m_speed_pid.reset();
        if (spd > hw::EBRAKE_MIN_KMH)
        {
            cmd = m_brake_pid.update(0.f, spd, hw::CTRL_DT_S,
                                     cfg.pid_kp, cfg.pid_ki, cfg.pid_kd,
                                     -1.f, 1.f);
            board::motorsSet(cmd, cmd, cap);
            ebrake = (cmd < 0.f);
        }
        else
        {
            board::motorsStop();
            m_brake_pid.reset();
        }
    }
    else
    {
        // Accélérateur → PWM direct (boucle ouverte). Un PID de **limitation de vitesse**
        // plafonne le PWM autorisé : tant que vitesse < limite il vaut 1 (aucun effet) ;
        // au-delà, il réduit le maximum permis.
        m_brake_pid.reset();
        float vmax = m_speed_pid.update(cfg.speed_limit_kmh, spd, hw::CTRL_DT_S,
                                        cfg.vmax_kp, cfg.vmax_ki, cfg.vmax_kd, 0.f, 1.f);
        float mag = std::min(m_thr_out, vmax);   // l'accélérateur fixe le PWM, plafonné par le PID
        cmd = m_rev_active ? -(mag * hw::REVERSE_FACTOR) : mag;
        board::motorsSet(cmd, cmd, cap);
    }

    // Détection de panne encodeur / blocage (PWM actif sans rotation > 1 s)
    updateEncStuck(now, cmd, spd, m_stuck_us);

    g_status.m_throttle.store(m_thr_out);
    g_status.m_brake.store(ebrake);
    g_status.m_out_l.store(cmd);
    g_status.m_out_r.store(cmd);
    g_status.m_rev_led.store(m_rev_active);
    board::reverseLED(m_rev_active);
    publish(spd, vbat);
}

void controlTask(void*)
{
    esp_task_wdt_add(nullptr);
    TickType_t last = xTaskGetTickCount();
    while (true)
    {
        tick();
        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(hw::CTRL_DT_MS));
    }
}
} // namespace

void kartInit()
{
    board::init();
    setState(State::Lockout, Fault::None);
}

void kartStart()
{
    // Tâche prioritaire épinglée sur le cœur applicatif (watchdog 5 s via sdkconfig).
    xTaskCreatePinnedToCore(controlTask, rtos::CONTROL.name, rtos::CONTROL.stack, nullptr,
                            rtos::CONTROL.prio, nullptr, rtos::CONTROL.core);
}
