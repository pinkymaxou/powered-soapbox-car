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
    // Timed separately from the whole tick: loop_max_us is WALL CLOCK and so also counts
    // preemption by Wi-Fi/BT, which would otherwise be blamed on the sensors. This one is
    // the I2C cost alone.
    const int64_t t0 = esp_timer_get_time();
    SensorReadings s;
    s.enc_delta_l = board::encLeftDelta();
    s.enc_delta_r = board::encRightDelta();
    s.enc_ok_l = board::encLeftPresent();
    s.enc_ok_r = board::encRightPresent();
    board::refreshMagStatus();                 // poll AS5600 STATUS (rate-limited internally)
    s.mag_ok_l = board::encLeftMagOk();
    s.mag_ok_r = board::encRightMagOk();
    // Read Vbat only at ~20 Hz (VBAT_READ_TICKS), not every tick: the ADS1115 shares I2C bus 0
    // with the LEFT AS5600, and polling it (×ADC_OVERSAMPLE) every tick jittered the left
    // encoder's read timing → RPM dips. The control loop only consumes Vbat at 20 Hz anyway.
    // One ADS1115 read per tick for ADC_OVERSAMPLE ticks out of every VBAT_READ_TICKS, rather
    // than all of them on one tick: same average, same ~20 Hz refresh, a fraction of the peak.
    if ((m_vbat_tick++ % hw::VBAT_READ_TICKS) < hw::ADC_OVERSAMPLE) m_pin_v = board::vbatSample();
    s.motor_pwr = board::motorPowerLive();
    s.vbat_ok = (m_pin_v >= 0.f);
    s.vbat_v = s.vbat_ok ? m_pin_v * hw::VBAT_DIV_RATIO : -1.f;
    const uint32_t d = static_cast<uint32_t>(esp_timer_get_time() - t0);
    for (uint32_t& peak : m_sens_max_us)
        if (d > peak) peak = d;
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
    st.m_idle_off_s = m_idle_off.remainingS();
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
    // Worst tick since the last read, published in the telemetry. A sensor that goes quiet
    // costs an I2C timeout, and this is what makes that cost VISIBLE instead of suspected.
    const int64_t t_begin = esp_timer_get_time();
    board::pollButtons();               // sampling/debounce of the START button
    pushPad();
    m_ctrl.setStartButton(board::btnStart());
    const KartConfig cfg = configSnapshot();   // the web config can change at any time
    const int cfg_idle_min = cfg.idle_off_min;
    m_ctrl.setConfig(cfg);
    const int64_t now = esp_timer_get_time();
    m_ctrl.tick(now);

    // HOST decisions derived from the telemetry (outside the core, see advisors.hpp).
    const CtrlTelemetry t = m_ctrl.telemetry();
    const RumbleCmd r = m_rumble.update(t, m_pad_in, now);
    if (r.active) input::rumble(r.strong, r.weak, r.duration_ms);
    if (m_poweroff.update(t, now)) board::powerOff();
    // Idle cutoff: fires ONCE. If the power does not actually drop (hold capacitor, or a
    // self-holding relay), we stay alive with the countdown parked at 0 rather than retrying.
    if (m_idle_off.update(t.armed, cfg_idle_min, now)) board::powerOff();

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

uint32_t EspController::sensMaxUs(int who)
{
    if (who < 0 || who >= PEAK_N) return 0;
    const uint32_t v = m_sens_max_us[who];
    m_sens_max_us[who] = 0;
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

uint32_t sensMaxUs(int who)
{
    return m_controller.sensMaxUs(who);
}

void start()
{
    xTaskCreatePinnedToCore(controlTask, rtos::CONTROL.name, rtos::CONTROL.stack, nullptr,
                            rtos::CONTROL.prio, nullptr, rtos::CONTROL.core);
}
} // namespace Controller
