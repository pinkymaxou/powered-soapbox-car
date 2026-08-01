# FreeRTOS tasks — priorities & core (ESP32, kart firmware)

ESP32 = **2 cores**: **core 0 = PRO_CPU** (network/system), **core 1 = APP_CPU** (application).
FreeRTOS at **1000 Hz**; priorities **0 (idle) → 24 (max)**, a **higher number = higher priority**.

## Application tasks (created by the firmware)

| Task | Priority | Core | Stack (B) | Period | Role | Source |
|---|:--:|:--:|:--:|---|---|---|
| **`control`** | **18** | **1** (APP) | 6144 | **500 Hz** | Control loop: pluggable **stick→motor mixing** (`mixer.hpp`), **rollover protection**, active (PID) braking + speed limiter, LVC, arming/fault state machine, event-log pushes (RAM ring only); **subscribed to the 2 s watchdog (PANIC)** | `controller.cpp` (`Controller::start`) |
| **`leds`** | **3** | **0** (PRO) | 3072 | ~20 Hz | Status display on the WS2812B strip (RMT) **+ event-log drain** (`evlog::maintain()`: RAM ring → flash, only while disarmed) | `leds.cpp` (`ledsStart`) |
| **`bt`** | **5** | **0** (PRO) | 8192 | (loop) | **BTstack / Bluepad32 loop**: Bluetooth stack, pairing and gamepad frames | `input_bp32.c` (`inputbp_start`) |

> **Why `control` sits at 18** (it started life at 6): host-side Bluetooth work on core 1
> preempted the loop long enough to stretch the interval between two AS5600 reads past the
> sensor's ½-turn window — the absolute angle then aliases and the measured speed flips sign
> in bursts. 18 keeps the loop under only the system tier (esp_timer 22, Wi-Fi/BT 23, IPC 24),
> and it yields every cycle (`vTaskDelayUntil`), so it starves nothing.
> The `bt` task runs `btstack_run_loop_execute()` (blocking); the BT stack additionally
> creates its own system tasks (BT controller, BTC/BTU) on core 0.
> There is deliberately **no** dedicated event-log task: its first version cost 3 kB of stack
> on a heap-starved board — the LED task's 20 Hz loop hosts the drain instead.

## ESP-IDF framework tasks (created automatically, dependencies)

Values = **IDF defaults** (configurable in sdkconfig); listed to situate the relative priorities.

| Task | Priority | Core | Stack (B) | Role |
|---|:--:|:--:|:--:|---|
| `esp_timer` | 22 | 0 | 3584 | High-resolution timer callbacks — runs our **STA reconnection** (`sta_retry`) and the **history sampler** (`histSample`, 1 s, never blocks: try-lock only) |
| `wifi` | 23 | 0 | ~3584 | Wi-Fi stack (MAC) |
| `tiT` (lwIP / tcpip) | 18 | 0 | 3072 | TCP/IP stack (same number as `control`, but the other core) |
| `sys_evt` | 20 | 0 | 2304 | Default event loop (`esp_event`) — receives WIFI_EVENT / IP_EVENT |
| `httpd` | 5 | no affinity | **8192** | HTTP/WebSocket server (raised from the 4096 default after a stack overflow) |
| `mdns` | 1 | 0 | 4096 | mDNS responder (`kart.local`) |
| `main` | 1 | 0 | 3584 | `app_main`: subsystem init then exits |
| `ipc0` / `ipc1` | 24 | 0 / 1 | 1024 | Inter-core IPC (system) |
| `IDLE0` / `IDLE1` | 0 | 0 / 1 | 1536 | Idle tasks — **watched by the watchdog** (sdkconfig) |

## Notes

- The **network (Wi-Fi 23, tcpip 18, httpd 5)** lives on **core 0**, away from the control
  loop (core 1). A web request cannot delay the control loop — what CAN delay it (any task,
  any core) is a **flash write** (the cache suspends): that is why every NVS-writing verb
  (`set`, `wifiset`, calibration, pairing) is **refused while armed**, and why the event log
  drains to flash **only while disarmed**.
- The **Wi-Fi STA reconnection** (every 5 s) runs in the context of the `esp_timer` task
  (not a dedicated task).
- **Watchdog (TWDT, 2 s, `CONFIG_ESP_TASK_WDT_PANIC=y`)**: the `control` task re-arms it every
  cycle; a block > 2 s **reboots** (a warning would leave the motors on their last PWM). The
  idle tasks are watched too.

> **Source of truth for the application tasks: [`firmware/main/rtos.hpp`](../firmware/main/rtos.hpp)**
> (priority, core, stack as `constexpr` constants). The watchdog timeout lives in
> `sdkconfig.defaults` (`CONFIG_ESP_TASK_WDT_TIMEOUT_S`). Keep this table aligned with both.
