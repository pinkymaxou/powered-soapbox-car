// controller.cpp — Liaison MATÉRIELLE du cœur de contrôle (voir controller_core.hpp).
// EspController branche les callbacks io* sur board::/input::/esp_timer, publie la
// télémétrie dans g_status (atomics lus par le webserver et les LED) et fait tourner
// la boucle à 500 Hz sous watchdog. TOUTE la logique vit dans controller_core.cpp —
// identique en simulation (test_host/sim).
#include "controller.hpp"

#include "config.hpp"
#include "controller_core.hpp"
#include "hardware.hpp"
#include "input.hpp"
#include "rtos.hpp"

#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{

class EspController final : public ControllerBase
{
protected:
    int64_t ioNowUs() override { return esp_timer_get_time(); }

    CtrlPad ioPad() override
    {
        m_in = input::get();   // instantané complet, gardé pour publier les champs d'affichage
        CtrlPad p;
        p.x = m_in.x;
        p.y = m_in.y;
        p.rx = m_in.rx;
        p.ry = m_in.ry;
        p.connected = m_in.connected;
        p.estop = m_in.estop;
        p.start = m_in.start;
        return p;
    }

    int64_t ioPadLastReportUs() override { return input::lastReportUs(); }
    bool    ioPadCalibrated() override   { return input::calibrated(); }
    bool    ioBtnStart() override        { return board::btnStart(); }
    float   ioVbatRaw() override         { return board::vbatVolts(hw::ADC_OVERSAMPLE); }
    int     ioEncDeltaL() override       { return board::encLeftDelta(); }
    int     ioEncDeltaR() override       { return board::encRightDelta(); }
    bool    ioEncPresentL() override     { return board::encLeftPresent(); }
    bool    ioEncPresentR() override     { return board::encRightPresent(); }

    void ioMotorsSet(float l, float r, uint32_t cap) override { board::motorsSet(l, r, cap); }
    void ioMotorsBrake() override                             { board::motorsBrake(); }
    void ioRumble(uint8_t s, uint8_t w, uint16_t ms) override { input::rumble(s, w, ms); }
    void ioPowerOff() override                                { board::powerOff(); }
    void ioFlushPendingConfig() override                      { configFlushPending(); }

public:
    // Publie la télémétrie du tick + les champs d'affichage manette (hors logique).
    void publish()
    {
        const CtrlTelemetry& t = telemetry();
        g_status.m_state.store(static_cast<int>(t.state));
        g_status.m_fault.store(static_cast<int>(t.fault));
        g_status.m_faults.store(t.faults);
        g_status.m_vbat.store(t.vbat);
        g_status.m_batt_type.store(t.batt_type);
        g_status.m_speed_l.store(t.speed_l);
        g_status.m_speed_r.store(t.speed_r);
        g_status.m_speed_ms.store(t.speed_ms);
        g_status.m_fwd.store(t.fwd);
        g_status.m_turn.store(t.turn);
        g_status.m_out_l.store(t.out_l);
        g_status.m_out_r.store(t.out_r);
        g_status.m_brake_mode.store(static_cast<int>(t.brake_mode));
        g_status.m_arming.store(t.armed);
        g_status.m_btn_start.store(t.btn_start);

        g_status.m_estop.store(m_in.estop);
        g_status.m_pad_conn.store(m_in.connected);
        g_status.m_pad_batt.store(input::battery());
        g_status.m_pad_x.store(m_in.rx);     // position physique du stick (cercle)
        g_status.m_pad_y.store(m_in.ry);
        g_status.m_pad_cx.store(m_in.x);     // consigne compensée cercle→carré
        g_status.m_pad_cy.store(m_in.y);
        g_status.m_pad_zl.store(m_in.zl);
        g_status.m_pad_zr.store(m_in.zr);
        g_status.m_pad_rx2.store(m_in.rx2);
        g_status.m_pad_ry2.store(m_in.ry2);
        g_status.m_pad_btns.store(m_in.buttons);
    }

private:
    input::State m_in;   // dernier instantané complet (ioPad le rafraîchit à chaque tick)
};

EspController m_ctrl;

void controlTask(void*)
{
    esp_task_wdt_add(nullptr);
    TickType_t last = xTaskGetTickCount();
    while (true)
    {
        board::pollButtons();               // échantillonnage/anti-rebond du bouton START
        m_ctrl.tick(configSnapshot());
        m_ctrl.publish();
        esp_task_wdt_reset();
        vTaskDelayUntil(&last, pdMS_TO_TICKS(hw::CTRL_DT_MS));
    }
}
} // namespace

void Controller::init()
{
    board::init();
    input::init();
    g_status.m_state.store(static_cast<int>(State::Lockout));
    g_status.m_fault.store(static_cast<int>(Fault::None));
    g_status.m_brake_mode.store(static_cast<int>(BrakeMode::Dynamic));   // défaut : freinage (jamais en roue libre)
}

void Controller::start()
{
    xTaskCreatePinnedToCore(controlTask, rtos::CONTROL.name, rtos::CONTROL.stack, nullptr,
                            rtos::CONTROL.prio, nullptr, rtos::CONTROL.core);
}
