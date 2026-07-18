// controller.cpp — Implémentation d'EspController + amorçage (namespace Controller).
#include "controller.hpp"

#include "config.hpp"
#include "hardware.hpp"
#include "rtos.hpp"

#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Callback capteurs : encodeurs + tension batterie — les ERREURS de lecture voyagent dans
// le retour (enc_ok_*/vbat_ok), et la tension part en VOLTS BATTERIE (la conversion broche
// ADC → batterie, ratio du diviseur, se fait ICI, pas dans le cœur).
SensorReadings EspController::readSensors()
{
    SensorReadings s;
    s.enc_delta_l = board::encLeftDelta();
    s.enc_delta_r = board::encRightDelta();
    s.enc_ok_l = board::encLeftPresent();
    s.enc_ok_r = board::encRightPresent();
    const float pin_v = board::vbatVolts(hw::ADC_OVERSAMPLE);
    s.vbat_ok = (pin_v >= 0.f);
    s.vbat_v = s.vbat_ok ? pin_v * m_ctrl.config().vbat_div_ratio : -1.f;
    return s;
}

// Callback sorties : la commande moteur, rien d'autre (voir CtrlOutputs).
void EspController::applyOutputs(const CtrlOutputs& out)
{
    if (out.dyn_brake) board::motorsBrake();
    else               board::motorsSet(out.out_l, out.out_r, out.cap);
}

// Pousse le dernier état manette au cœur (fonction d'entrée setPad).
void EspController::pushPad()
{
    m_in = input::get();   // instantané complet, gardé pour publier les champs d'affichage
    PadInputs p;
    p.x = m_in.x;
    p.y = m_in.y;
    p.rx = m_in.rx;
    p.ry = m_in.ry;
    p.connected = m_in.connected;
    p.calibrated = input::calibrated();
    p.estop = m_in.estop;
    p.start = m_in.start;
    p.last_report_us = input::lastReportUs();
    m_ctrl.setPad(p);
    m_pad_in = p;
}

// Publie la télémétrie du tick + les champs d'affichage manette (hors logique).
void EspController::publish(const CtrlTelemetry& t)
{
    KartStatus st;
    st.m_state      = static_cast<int>(t.state);
    st.m_fault      = static_cast<int>(primaryFault(t.faults));   // dérivé du bitset
    st.m_faults     = t.faults;
    st.m_vbat       = t.vbat;
    st.m_batt_type  = t.batt_type;
    st.m_rpm_l      = t.rpm_l;
    st.m_rpm_r      = t.rpm_r;
    st.m_speed_ms   = t.speed_ms;
    st.m_fwd        = t.fwd;
    st.m_turn       = t.turn;
    st.m_out_l      = t.out_l;
    st.m_out_r      = t.out_r;
    st.m_brake_mode = static_cast<int>(t.brake_mode);
    st.m_arming     = t.armed;
    st.m_btn_start  = t.btn_start;

    st.m_estop    = m_in.estop;
    st.m_pad_conn = m_in.connected;
    st.m_pad_batt = input::battery();
    st.m_pad_x    = m_in.rx;      // position physique du stick (cercle)
    st.m_pad_y    = m_in.ry;
    st.m_pad_cx   = m_in.x;       // consigne compensée cercle→carré
    st.m_pad_cy   = m_in.y;
    st.m_pad_zl   = m_in.zl;
    st.m_pad_zr   = m_in.zr;
    st.m_pad_rx2  = m_in.rx2;
    st.m_pad_ry2  = m_in.ry2;
    st.m_pad_btns = m_in.buttons;
    statusPublish(st);
}

void EspController::init()
{
    board::init();
    input::init();
    m_ctrl.setCallbacks([this] { return readSensors(); },
                        [this](const CtrlOutputs& out) { applyOutputs(out); });
    statusPublish(KartStatus{});   // défauts sûrs : Lockout, frein dynamique, aucun défaut
}

void EspController::tickOnce()
{
    board::pollButtons();               // échantillonnage/anti-rebond du bouton START
    pushPad();
    m_ctrl.setStartButton(board::btnStart());
    m_ctrl.setConfig(configSnapshot());   // la config web peut changer à tout moment
    const int64_t now = esp_timer_get_time();
    m_ctrl.tick(now);

    // Décisions d'HÔTE dérivées de la télémétrie (hors du cœur, voir advisors.hpp).
    const CtrlTelemetry t = m_ctrl.telemetry();
    const RumbleCmd r = m_rumble.update(t, m_pad_in, now);
    if (r.active) input::rumble(r.strong, r.weak, r.duration_ms);
    if (m_poweroff.update(t, now)) board::powerOff();
    if (m_was_armed && !t.armed) configFlushPending();   // « set » différé reçu en roulant
    m_was_armed = t.armed;

    publish(t);
}

// ── Amorçage : l'instance, la tâche 500 Hz, la boucle sur tickOnce(). Rien d'autre. ──
namespace
{
EspController m_controller;

void controlTask(void*)
{
    esp_task_wdt_add(nullptr);
    TickType_t last = xTaskGetTickCount();
    while (true)
    {
        m_controller.tickOnce();
        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(hw::CTRL_DT_MS));
    }
}
} // namespace

namespace Controller
{
void init()
{
    m_controller.init();
}

void start()
{
    xTaskCreatePinnedToCore(controlTask, rtos::CONTROL.name, rtos::CONTROL.stack, nullptr,
                            rtos::CONTROL.prio, nullptr, rtos::CONTROL.core);
}
} // namespace Controller
