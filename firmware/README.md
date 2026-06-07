# Firmware ESP32 — Kart électrique 2 places

Firmware **ESP-IDF 6.1** (C++) pilotant les 2 moteurs via le driver double canal, avec
accélérateur, retour de vitesse (capteur d'angle I²C), sécurités, **ruban WS2812B** et **configuration par Wi-Fi**.

## Compilation / flash

```bash
. ~/esp/esp-idf-6.1/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Configuration sans fil (SoftAP + WebSocket)

Au démarrage, l'ESP32 crée un point d'accès :

- **SSID** : `Kart-Config`  ·  **mot de passe** : `kart12345`
- Ouvrir **http://192.168.4.1**

L'onglet **Wi-Fi** permet de saisir un SSID/mot de passe et d'**activer le mode station**
(case à cocher) : le kart se connecte alors à ce réseau **tout en gardant le SoftAP**
(mode AP+STA). Prise en compte **au redémarrage** ; reconnexion automatique toutes les 5 s.

La page (5 onglets : **Tableau de bord / Configuration / Calibration / Wi-Fi / Brochage**)
communique par **WebSocket** (`/ws`). État live à 4 Hz (badge d'état dans le titre, barres
de progression, pastilles d'E/S) + **graphiques** alimentés par un **historique conservé en RAM**
côté ESP32 (persiste page fermée) : principal **10 min** (accél/PWM/vitesse, 1 pt/s) et
**batterie 30 min** (canvas séparé, 1 pt/5 s). Configuration : formulaire **auto-généré** depuis
la table de paramètres, persisté en flash. Calibration : déclenchée via le web (pas de bouton).

## Architecture

| Fichier | Rôle |
|---|---|
| `pinout.hpp` | **Brochage matériel** (broches fixes) |
| `config.hpp` / `.cpp` | **Table de paramètres** `PARAMS[]` + `KartConfig` (persistée NVS, clé par clé) + télémétrie `KartStatus` |
| `hardware.hpp` / `.cpp` | Accès matériel bas niveau (`board::` — ADC, PWM/DIR, capteur d'angle AS5600 I²C, boutons, LED) |
| `controller.hpp` / `.cpp` | **Boucle de contrôle** (namespace, tâche FreeRTOS 500 Hz) : machine à états + sécurités |
| `pid.hpp` | Régulateur **PID** réutilisable avec **anti-windup** |
| `leds.hpp` / `.cpp` | **Tâche dédiée** au ruban WS2812B (affichage d'état) |
| `ws2812.hpp` / `.cpp` | Pilote WS2812B (RMT) |
| `webserver.hpp` / `.cpp` | SoftAP + serveur **HTTP/WebSocket** |
| `assets/` | Page web `index.html` + `style.css` (embarqués) |
| `main.cpp` | `app_main` : init des sous-systèmes + démarrage des tâches |

Deux tâches : **`control`** (cœur 1, 500 Hz, watchdog 5 s) et **`leds`** (cœur 0, ~20 Hz).
Elles partagent `g_cfg` (mutex) et `g_status` (atomics). FreeRTOS tourne à **1000 Hz**.

## Boucle de contrôle (500 Hz)

Accélérateur (ADC, calibré) → **PWM direct** (boucle ouverte, rampé, plafonné ~50 %) → driver
→ **les 2 moteurs reçoivent la même commande**. Relâcher l'accélérateur ⇒ **frein PID** qui
ramène la vitesse à **0**. Vitesse mesurée par le **capteur d'angle AS5600** (I²C, sur l'essieu) :
**dérivée de l'angle 12 bits** avec gestion du wrap ; le **signe de Δ** donne le sens (sortie PID
signée → peut inverser le moteur). Retour batterie **Vbat** (LVC anti-sag). Marche arrière par bouton momentané.

Sécurités : **armement** par appui ~1 s sur START (démarrage **désarmé**), **désarmement auto**
après 30 s sans accélérateur, anti-démarrage pédale enfoncée, détection de défaut accélérateur,
**coupure basse tension (LVC)** avec hystérésis, **watchdog 5 s** (reboot si blocage),
**PWM plafonné ~50 %** (moteurs 12 V sur batterie 20 V).

## ⚠️ À ajuster avant la première mise en route

Dans `config.hpp` (`namespace hw`, **hardcodé**) : `WHEEL_DIAM_M` (diamètre roue réel) — seul
paramètre du capteur à confirmer (AS5600 sur l'essieu → `GEAR_RATIO = 1`, `AS5600_CPR = 4096`).
Via la page web : calibrer l'accélérateur, ajuster `vbat_div_ratio` au multimètre,
régler la limite de vitesse. Commencer **roues en l'air**, vitesse basse.

**Pré-réglage PID (hypothèse de bring-up, à affiner au banc).** Estimé depuis le modèle
(gain ≈ 8 km/h/commande à 50 % PWM via 1:16 + roue Ø0,30 m ; τ ≈ 1,5 s à ~100 kg) :
**limiteur** `vmax_kp≈0,35`, `vmax_ki≈0,25`, `vmax_kd=0` (réglage IMC, λ≈0,5 s) ;
**frein** `pid_kp≈0,10`, `pid_ki≈0,05`, `pid_kd≈0,003` (arrêt ferme sans plugging brutal).
Le PID intègre `dt` → **passer de 100 à 500 Hz ne change pas Kp/Ki** ; garder **Kd petit**
(dérivé plus bruité à 500 Hz). ⚠️ Valable **uniquement après calibration de `WHEEL_DIAM_M`**.
Voir aussi la section **« Limitations et risques connus »** du [README racine](../README.md).
