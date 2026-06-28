# Tâches FreeRTOS — priorités & cœur (ESP32, firmware kart)

ESP32 = **2 cœurs** : **cœur 0 = PRO_CPU** (réseau/système), **cœur 1 = APP_CPU** (applicatif).
FreeRTOS à **1000 Hz** ; priorités **0 (idle) → 24 (max)**, un nombre **plus élevé = plus prioritaire**.

## Tâches applicatives (créées par le firmware)

| Tâche | Priorité | Cœur | Pile (o) | Période | Rôle | Source |
|---|:--:|:--:|:--:|---|---|---|
| **`control`** | **6** | **1** (APP) | 6144 | **500 Hz** | Boucle d'asservissement : **mélange différentiel** (manette→2 PWM), **anti-renversement**, frein PID + limiteur **par roue**, LVC, machine à états ; **abonnée au watchdog 5 s** | `controller.cpp` (`kartStart`) |
| **`leds`** | **3** | **0** (PRO) | 3072 | ~20 Hz | Affichage d'état sur le ruban WS2812B (RMT) | `leds.cpp` (`ledsStart`) |
| **`bt`** | **5** | **0** (PRO) | 8192 | (boucle) | **Boucle BTstack / Bluepad32** : pile Bluetooth, appairage et trames manette | `input_bp32.c` (`inputbp_start`) |

> `control` et `leds` sont créées par `xTaskCreatePinnedToCore(...)`. Le **contrôle est isolé sur le cœur 1** pour ne pas être perturbé par les piles Wi-Fi/réseau **et Bluetooth** (cœur 0) → cadence 500 Hz régulière.
> La tâche `bt` exécute `btstack_run_loop_execute()` (bloquante) ; la pile BT crée en plus ses propres tâches système (contrôleur BT, BTC/BTU) sur le cœur 0.

## Tâches du framework ESP-IDF (créées automatiquement, dépendances)

Valeurs = **défauts IDF** (réglables en sdkconfig) ; listées pour situer les priorités relatives.

| Tâche | Priorité | Cœur | Pile (o) | Rôle |
|---|:--:|:--:|:--:|---|
| `esp_timer` | 22 | 0 | 3584 | Callbacks de minuterie haute résolution — exécute notre **reconnexion STA** (`sta_retry`) |
| `wifi` | 23 | 0 | ~3584 | Pile Wi-Fi (MAC) |
| `tiT` (lwIP / tcpip) | 18 | 0 | 3072 | Pile TCP/IP |
| `sys_evt` | 20 | 0 | 2304 | Boucle d'événements par défaut (`esp_event`) — reçoit WIFI_EVENT / IP_EVENT |
| `httpd` | 5 | sans affinité | 4096 | Serveur HTTP/WebSocket (config web + page Système) |
| `main` | 1 | 0 | 3584 | `app_main` : init des sous-systèmes puis se termine |
| `ipc0` / `ipc1` | 24 | 0 / 1 | 1024 | IPC inter-cœurs (système) |
| `IDLE0` / `IDLE1` | 0 | 0 / 1 | 1536 | Tâches idle — **surveillées par le watchdog** (sdkconfig) |

## Notes

- **`control` (6) > `leds` (3)** : si le cœur 1 était partagé, le contrôle primerait l'affichage ; ici ils sont sur des cœurs différents de toute façon.
- Le **réseau (Wi-Fi 23, tcpip 18, httpd 5)** vit sur le **cœur 0**, à l'écart de la boucle de contrôle (cœur 1). Une requête web ne peut donc pas retarder l'asservissement.
- La **reconnexion Wi-Fi STA** (toutes les 5 s) s'exécute dans le contexte de la tâche `esp_timer` (pas une tâche dédiée).
- **Watchdog (TWDT, 5 s)** : la tâche `control` le réarme à chaque tour ; les tâches idle sont aussi surveillées → un blocage > 5 s déclenche un reboot.

> **Source de vérité des tâches applicatives : [`firmware/main/rtos.hpp`](../firmware/main/rtos.hpp)** (priorité, cœur, pile en constantes `constexpr`, référencées par `controller.cpp` et `leds.cpp`). Tenir ce tableau aligné sur ce fichier.
