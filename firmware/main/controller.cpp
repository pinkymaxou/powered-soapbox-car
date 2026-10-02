// controller.cpp — EspController implementation + bootstrap (Controller namespace).
#include "controller.hpp"

#include "config.hpp"
#include "evlog.hpp"
#include "hardware.hpp"
#include "rtos.hpp"

#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
    p.drive = m_in.drive;
    p.last_report_us = input::lastReportUs();
    m_ctrl.setPad(p);
    m_pad_in = p;
}

// Publishes the tick telemetry + the gamepad display fields (outside the logic).
void EspController::publish(const CtrlTelemetry& t)
{
    KartStatus st;
    st.m_state      = static_cast<int>(t.state);
    st.m_stop       = static_cast<int>(t.stop);
    st.m_fwd        = t.fwd;
    st.m_turn       = t.turn;
    st.m_out_l      = t.out_l;
    st.m_out_r      = t.out_r;
    st.m_brake_mode = static_cast<int>(t.brake_mode);
    st.m_arming     = t.armed;
    st.m_btn_start  = t.btn_start;
    st.m_btn_drive  = m_in.drive;

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
    m_ctrl.setCallbacks([this](const CtrlOutputs& out) { applyOutputs(out); });
    statusPublish(KartStatus{});   // safe defaults: Lockout, dynamic braking, no fault
}

void EspController::tickOnce()
{
    // Worst tick since the last read, published in the telemetry.
    const int64_t t_begin = esp_timer_get_time();
    pushPad();
    const KartConfig cfg = configSnapshot();   // the web config can change at any time
    m_ctrl.setConfig(cfg);
    const int64_t now = esp_timer_get_time();
    m_ctrl.tick(now);

    // HOST decisions derived from the telemetry (outside the core, see advisors.hpp).
    const CtrlTelemetry t = m_ctrl.telemetry();
    const RumbleCmd r = m_rumble.update(t, m_pad_in, now);
    if (r.active) input::rumble(r.strong, r.weak, r.duration_ms);
    // Event log: the arm/disarm edges. The Disarm record carries the Stop cause of its very
    // tick — that IS the answer to "why did it stop": the culprit (pad stale, e-stop…) appears
    // on the tick that disarms, so watching for it separately would always miss it.
    // Stop::None = a manual disarm or the inactivity timeout.
    // push() is a RAM ring write — the flash cost lives in evlog's drain task, disarmed only.
    if (t.armed && !m_ev_armed) evlog::push(evlog::Ev::Arm, 0);
    if (!t.armed && m_ev_armed) evlog::push(evlog::Ev::Disarm, static_cast<uint32_t>(t.stop));
    m_ev_armed = t.armed;

    publish(t);

    const uint32_t dur = static_cast<uint32_t>(esp_timer_get_time() - t_begin);
    for (uint32_t& peak : m_loop_max_us)
        if (dur > peak) peak = dur;
}

uint32_t EspController::loopMaxUs(int who)
{
    if (who < 0 || who >= PEAK_N) return 0;
    const uint32_t v = m_loop_max_us[who];
    m_loop_max_us[who] = 0;
    return v;
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

uint32_t loopMaxUs(int who)
{
    return m_controller.loopMaxUs(who);
}

void start()
{
    xTaskCreatePinnedToCore(controlTask, rtos::CONTROL.name, rtos::CONTROL.stack, nullptr,
                            rtos::CONTROL.prio, nullptr, rtos::CONTROL.core);
}
} // namespace Controller
