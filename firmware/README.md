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
Priorités / cœurs / piles dans [`main/rtos.hpp`](main/rtos.hpp) — détail (+ tâches IDF) : [`../doc/firmware-tasks.md`](../doc/firmware-tasks.md).

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

Capteur de vitesse : cinématique **connue et hardcodée** dans `config.hpp` (`namespace hw`) —
`AS5600_CPR = 4096`, `GEAR_RATIO = 1` (capteur 1:16 depuis le moteur + courroie 1:1 = vitesse roue),
`WHEEL_DIAM_M = 0,3048` (roue 12″) → rien à mesurer, juste à **vérifier au banc**.
Via la page web : calibrer l'accélérateur, ajuster `vbat_div_ratio` au multimètre,
régler la limite de vitesse. Commencer **roues en l'air**, vitesse basse.

**Pré-réglage PID (extrapolé du modèle, à affiner au banc).** Gain plant **K ≈ 13 km/h/commande**
(12 V→4615 tr/min à ~10 V moyens via plafond 50 %, 1:16, roue 12″, charge légère) ;
τ ≈ 1,1 s (amortissement moteur ≈90 N·s/m à ~100 kg). Réglage IMC :
**limiteur** `vmax_kp≈0,15`, `vmax_ki≈0,14`, `vmax_kd=0` (λ≈0,55 s) ;
**frein** `pid_kp≈0,12`, `pid_ki≈0,08`, `pid_kd≈0,003` (sature en plugging > ~8 km/h, doux près de 0).
Le PID intègre `dt` → **passer de 100 à 500 Hz ne change pas Kp/Ki** ; garder **Kd petit**
(dérivé plus bruité à 500 Hz). La conversion vitesse étant désormais exacte, ces gains
sont **directement applicables** (à affiner au banc selon le comportement réel).
Voir aussi la section **« Limitations et risques connus »** du [README racine](../README.md).
