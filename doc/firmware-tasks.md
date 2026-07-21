# FreeRTOS tasks — priorities & core (ESP32, kart firmware)

ESP32 = **2 cores**: **core 0 = PRO_CPU** (network/system), **core 1 = APP_CPU** (application).
FreeRTOS at **1000 Hz**; priorities **0 (idle) → 24 (max)**, a **higher number = higher priority**.

## Application tasks (created by the firmware)

| Task | Priority | Core | Stack (B) | Period | Role | Source |
|---|:--:|:--:|:--:|---|---|---|
| **`control`** | **6** | **1** (APP) | 6144 | **500 Hz** | Control loop: **differential mixing** (gamepad→2 PWM), **rollover protection**, active (PID) braking + limiter **per wheel**, LVC, state machine; **subscribed to the 5 s watchdog** | `controller.cpp` (`kartStart`) |
| **`leds`** | **3** | **0** (PRO) | 3072 | ~20 Hz | Status display on the WS2812B strip (RMT) | `leds.cpp` (`ledsStart`) |
| **`bt`** | **5** | **0** (PRO) | 8192 | (loop) | **BTstack / Bluepad32 loop**: Bluetooth stack, pairing and gamepad frames | `input_bp32.c` (`inputbp_start`) |

> `control` and `leds` are created by `xTaskCreatePinnedToCore(...)`. **Control is isolated on core 1** so it is not disturbed by the Wi-Fi/network **and Bluetooth** stacks (core 0) → a steady 500 Hz rate.
> The `bt` task runs `btstack_run_loop_execute()` (blocking); the BT stack additionally creates its own system tasks (BT controller, BTC/BTU) on core 0.

## ESP-IDF framework tasks (created automatically, dependencies)

Values = **IDF defaults** (configurable in sdkconfig); listed to situate the relative priorities.

| Task | Priority | Core | Stack (B) | Role |
|---|:--:|:--:|:--:|---|
| `esp_timer` | 22 | 0 | 3584 | High-resolution timer callbacks — runs our **STA reconnection** (`sta_retry`) |
| `wifi` | 23 | 0 | ~3584 | Wi-Fi stack (MAC) |
| `tiT` (lwIP / tcpip) | 18 | 0 | 3072 | TCP/IP stack |
| `sys_evt` | 20 | 0 | 2304 | Default event loop (`esp_event`) — receives WIFI_EVENT / IP_EVENT |
| `httpd` | 5 | no affinity | 4096 | HTTP/WebSocket server (web config + System page) |
| `main` | 1 | 0 | 3584 | `app_main`: subsystem init then exits |
| `ipc0` / `ipc1` | 24 | 0 / 1 | 1024 | Inter-core IPC (system) |
| `IDLE0` / `IDLE1` | 0 | 0 / 1 | 1536 | Idle tasks — **watched by the watchdog** (sdkconfig) |

## Notes

- **`control` (6) > `leds` (3)**: if core 1 were shared, control would take precedence over the display; here they are on different cores anyway.
- The **network (Wi-Fi 23, tcpip 18, httpd 5)** lives on **core 0**, away from the control loop (core 1). A web request therefore cannot delay the control loop.
- The **Wi-Fi STA reconnection** (every 5 s) runs in the context of the `esp_timer` task (not a dedicated task).
- **Watchdog (TWDT, 5 s)**: the `control` task re-arms it every cycle; the idle tasks are also watched → a block > 5 s triggers a reboot.

> **Source of truth for the application tasks: [`firmware/main/rtos.hpp`](../firmware/main/rtos.hpp)** (priority, core, stack as `constexpr` constants, referenced by `controller.cpp` and `leds.cpp`). Keep this table aligned with that file.
