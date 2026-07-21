// controller.cpp — EspController implementation + bootstrap (Controller namespace).
#include "controller.hpp"

#include "config.hpp"
#include "hardware.hpp"
#include "rtos.hpp"

#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Sensor callback: encoders + battery voltage — the read ERRORS travel in
// the return (enc_ok_*/vbat_ok), and the voltage leaves in BATTERY VOLTS (the ADC pin →
// battery conversion, divider ratio, is done HERE, not in the core).
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

// Output callback: the motor command, nothing else (see CtrlOutputs).
void EspController::applyOutputs(const CtrlOutputs& out)
{
    if (out.dyn_brake) board::motorsBrake();
    else               board::motorsSet(out.out_l, out.out_r, out.cap);
}

// Pushes the last gamepad state to the core (setPad input function).
void EspController::pushPad()
{
    m_in = input::get();   // complete snapshot, kept to publish the display fields
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

// Publishes the tick telemetry + the gamepad display fields (outside the logic).
void EspController::publish(const CtrlTelemetry& t)
{
    KartStatus st;
    st.m_state      = static_cast<int>(t.state);
    st.m_fault      = static_cast<int>(primaryFault(t.faults));   // derived from the bitset
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
    st.m_pad_x    = m_in.rx;      // physical stick position (circle)
    st.m_pad_y    = m_in.ry;
    st.m_pad_cx   = m_in.x;       // compensated command circle→square
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
    statusPublish(KartStatus{});   // safe defaults: Lockout, dynamic braking, no fault
}

void EspController::tickOnce()
{
    board::pollButtons();               // sampling/debounce of the START button
    pushPad();
    m_ctrl.setStartButton(board::btnStart());
    m_ctrl.setConfig(configSnapshot());   // the web config can change at any time
    const int64_t now = esp_timer_get_time();
    m_ctrl.tick(now);

    // HOST decisions derived from the telemetry (outside the core, see advisors.hpp).
    const CtrlTelemetry t = m_ctrl.telemetry();
    const RumbleCmd r = m_rumble.update(t, m_pad_in, now);
    if (r.active) input::rumble(r.strong, r.weak, r.duration_ms);
    if (m_poweroff.update(t, now)) board::powerOff();
    if (m_was_armed && !t.armed) configFlushPending();   // deferred "set" received while driving
    m_was_armed = t.armed;

    publish(t);
}

// ── Bootstrap: the instance, the 500 Hz task, the loop over tickOnce(). Nothing else. ──
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
