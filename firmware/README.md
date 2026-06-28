# Firmware ESP32 — Kart à entraînement différentiel (branche `experiment`)

> ⚠️ **Branche expérimentale.** Cette branche explore une architecture **radicalement
> différente** du kart « de production » décrit dans le [README racine](../README.md)
> (4 roues, direction par bielle, 2 moteurs arrière, pédale accélérateur).
> Ici : **tricycle à entraînement différentiel**.

Firmware **ESP-IDF 6.1** (C++) pilotant **2 moteurs avant indépendants** (un par roue) :
la **direction se fait par différence de vitesse** entre les deux roues (*differential /
skid steer*). La **roue arrière est folle** (roulette pivotante libre). Commande par
**manette Bluetooth**, retour de vitesse par **2 capteurs d'angle AS5600** (un par roue,
sur 2 bus I²C), sécurités, **ruban WS2812B** et **configuration par Wi-Fi**.

## Architecture mécanique (rappel)

```
        AVANT
   🛞 G        🛞 D     ← 2 roues motrices, chacune son moteur + son AS5600
    \          /
     \        /         direction = différentiel de vitesse G/D
      \      /          (pivote sur place si avance ≈ 0)
        🛞               ← 1 roue arrière FOLLE (roulette pivotante)
       ARRIÈRE
```

- **Mélange « arcade »** : `gauche = avance − virage·gain`, `droite = avance + virage·gain`.
- **Anti-renversement** : un tricycle se renverse facilement → la **vitesse de virage est
  bridée en fonction de la vitesse d'avance** pour borner l'accélération latérale
  (`a_lat_max`). Voir [Anti-renversement](#anti-renversement).

## Compilation / flash

```bash
. ~/esp/esp-idf-6.1/export.sh
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

> **Composants vendorés** (dans [`components/`](components/), ignorés par git) : `bluepad32`,
> `btstack`, `cmd_nvs`, `cmd_system`. Bluepad32/BTstack ne sont pas dans le registre de
> composants → ils ont été clonés puis intégrés (`integrate_btstack.py`) et **adaptés à
> IDF 6.1** (le composant `driver` y est scindé en `esp_driver_*` ; sources legacy
> `uni_mouse_quadrature.c` / plateformes intégrées retirées, facteur d'échelle souris stubé).
> Voir [`components/bluepad32/CMakeLists.txt`](components/bluepad32/CMakeLists.txt).

### Bluetooth + Wi-Fi : configuration radio

L'ESP32 partage une **seule radio** entre Wi-Fi et Bluetooth (coexistence TDM) et l'app
grossit nettement (~1,4 Mo). Réglages dans [`sdkconfig.defaults`](sdkconfig.defaults) :

- **Table de partitions custom** ([`partitions.csv`](partitions.csv)) : `factory` ~2,75 Mo,
  **sans OTA** (flash 4 Mo) — il reste ~51 % libre.
- **BT activé** (`BT_ENABLED`, mode **BTDM** = BLE + BR/EDR, *modem sleep* désactivé) +
  **coexistence logicielle** (`ESP_COEX_SW_COEXIST_ENABLE`).
- **Bluepad32** : plateforme **CUSTOM** (`BLUEPAD32_PLATFORM_CUSTOM`), audio désactivé.
- **IRAM** : Wi-Fi + BT saturent l'IRAM → `ESP_WIFI_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT`
  désactivés (code Wi-Fi déplacé en flash ; impact négligeable sur la commande).

## Manette Bluetooth (Bluepad32)

Le backend ([`input_bp32.c`](main/input_bp32.c)) implémente une **plateforme Bluepad32
custom** et fait tourner la boucle **BTstack** dans une tâche dédiée (cœur 0). Les trames
manette sont transmises au firmware via des hooks C (`inputbp_on_data` / `inputbp_on_conn`)
consommés par [`input.cpp`](main/input.cpp), qui expose l'interface neutre
[`input.hpp`](main/input.hpp) (`input::get()` → `{x, y, connected, estop}`).

- **Stick gauche** : `Y` = avance/recul, `X` = virage (mélange arcade dans le contrôleur).
- **Compensation cercle→carré** : le stick est borné mécaniquement par un **cercle**
  (`x²+y²≤1`) → en diagonale pleine chaque axe plafonnerait à ~0,71. `input::get()` **étire
  radialement** la consigne (facteur `|v|/max(|x|,|y|)`, =√2 en diagonale) pour que les
  **coins du carré soient atteignables** : avance **et** virage à fond simultanément.
- **Bouton B** = **arrêt d'urgence** (frein immédiat).
- **Retour haptique** (`input::rumble`) : vibration **douce** à l'armement, **forte** sur
  erreur soudaine / e-stop, et **forte (répétée)** si on pousse le stick alors que le
  véhicule est bloqué (non armé, etc.). La requête est posée par la boucle de contrôle et
  jouée dans le thread BT (`play_dual_rumble`).
- **Appairage / désappairage** pilotés depuis la page web (onglet **Manette**).

### Calibration OBLIGATOIRE

**Le kart refuse de rouler tant que la manette n'est pas calibrée** (`input::get()`
renvoie des axes à 0 si non calibré → le contrôleur reste au frein). La calibration se
fait **exclusivement depuis la page web**, **pour la manette uniquement** :

1. **Centre** : manche au repos → capture le point neutre.
2. **Extrêmes** : bouger les sticks à fond → capture l'amplitude par axe.

L'échelle (centre + demi-amplitude par axe) est **persistée en NVS** (namespace `pad`).
⚠️ **Un (ré)appairage EFFACE la calibration** (nouvelle manette = nouvelle calibration).

### Sécurité : déconnexion → frein

Si la manette **se déconnecte** (hors de portée, batterie vide, désappairage), `connected`
passe à `false` et le contrôleur **met immédiatement les 2 moteurs en mode freinage**.
Idem si non armé, non calibré, arrêt d'urgence, ou défaut capteur/LVC.

## Mesures analogiques (ADC externe ADS1115)

Toutes les entrées analogiques passent par un **ADS1115** (16 bits, I²C, PGA) au lieu de
l'ADC interne de l'ESP32 — **plus précis et linéaire**, et sans le conflit ADC2/Wi-Fi.
Le breakout se branche **en piggyback sur le bus I²C 0** (avec l'AS5600 gauche : adresses
distinctes `0x36` / `0x48`). Driver dédié : [`ads1115.hpp`](main/ads1115.hpp).

- ⚠️ **Alimenter en 3,3 V** (niveaux I²C compatibles ESP32) → `AIN_max = 3,3 V`.
- **A0 = tension batterie** (via le pont diviseur 100k/15k), suivie en **mode continu**
  (±4,096 V, résolution 125 µV). `board::vbatVolts()` lit le registre de conversion.
- **A1 / A2 = réservés** au futur joystick X/Y (lecture single-shot) ; **A3 libre**.
- Adresse réglable par la broche ADDR (`0x48` GND … `0x4B` SCL) — voir `pinout.hpp`.

Le driver **dégrade proprement** : si l'ADS1115 est absent (non câblé), `begin()` le détecte
(probe I²C) et `vbatVolts()` renvoie 0 — aucun crash.

### Joystick physique (réservé, non implémenté)

La conception prévoit un **joystick physique** en alternative à la manette. Pour l'instant
**seul le Bluetooth est implémenté**, mais l'interface `input::` est neutre et **2 voies de
l'ADS1115 (A1/A2) sont réservées** ([`pinout.hpp`](main/pinout.hpp), `namespace pins::ads`).

### Encodeurs de roue (réservés, non câblés)

En plus des 2× AS5600, des broches sont **réservées « au cas où »** pour des **encodeurs
incrémentaux en quadrature AMT103-V** (un par roue avant), en alternative/complément
([`pinout.hpp`](main/pinout.hpp), `namespace pins::future`) :

| Encodeur | Canal A | Canal B |
|---|---|---|
| Roue gauche | GPIO34 | GPIO35 |
| Roue droite | GPIO36 | GPIO39 |

- Broches **input-only** (34/35/36/39) → idéales en entrée ; décodage matériel via **PCNT**.
- Sorties **CMOS push-pull** A/B (+ index X optionnel sur 22/23) — **aucun pull-up requis**.
- ⚠️ **Alimentation** : VDD min ~3,6 V → sortie haute ≈ 2,8 V, **lisible par l'ESP32 sans
  level-shift**. À **5 V** la sortie monte à ~4,2 V → **diviseur/level-shift obligatoire**.

## Configuration sans fil (SoftAP + WebSocket)

Au démarrage, l'ESP32 crée un point d'accès :

- **SSID** : `Kart-Config`  ·  **mot de passe** : `kart12345`
- Ouvrir **http://192.168.4.1**

L'onglet **Wi-Fi** permet de saisir un SSID/mot de passe et d'**activer le mode station**
(case à cocher) : le kart se connecte alors à ce réseau **tout en gardant le SoftAP**
(mode AP+STA). Prise en compte **au redémarrage** ; reconnexion automatique toutes les 5 s.

La page (6 onglets : **Tableau de bord / Configuration / Manette / Wi-Fi / Brochage /
Système**) communique par **WebSocket** (`/ws`). État live à 4 Hz (badge d'état, barres,
pastilles d'E/S) + **graphiques Chart.js gradués** alimentés par un **historique en RAM**
côté ESP32. Le graphe **Avance · PWM** affiche en plus le **régime (tr/min) de chaque roue
sur un 2ᵉ axe** (droite). L'onglet **Manette** regroupe : **bouton d'appairage**, **infos
manette** (nom, batterie, connexion), **bouton de désappairage**, le **mode calibration**,
et une **visualisation temps réel** : pavé 2D montrant **deux points** — la **position
physique** du stick gauche (bleu, sur le cercle) et la **consigne compensée** cercle→carré
(orange, atteint les coins du carré), reliés par un trait — un 2ᵉ pavé pour le **stick droit**
(affichage seul, non calibré), la **croix directionnelle (D-pad)**, les pastilles **d'état des
boutons**, les **barres des gâchettes ZL/ZR**, et le **masque brut (hex)** des boutons (pour
identifier les boutons spécifiques d'une manette).

## Architecture logicielle

| Fichier | Rôle |
|---|---|
| `pinout.hpp` | **Brochage matériel** (2 moteurs, 2 bus I²C, réserves joystick/futur) |
| `config.hpp` / `.cpp` | **Table de paramètres** `PARAMS[]` + `KartConfig` (NVS) + télémétrie `KartStatus` |
| `hardware.hpp` / `.cpp` | Matériel bas niveau (`board::` — Vbat via ADS1115, **2× PWM/DIR**, **2× AS5600** sur 2 bus I²C, boutons, LED, latch) |
| `ads1115.hpp` / `.cpp` | **Driver ADS1115** (ADC externe 16 bits I²C, PGA) — modes continu / single-shot, par canal |
| `input.hpp` / `.cpp` | **Entrée manette** (interface neutre) + **calibration obligatoire** (NVS) |
| `input_bp32.c` | **Backend Bluepad32/BTstack** (plateforme custom + tâche boucle BT) |
| `controller.hpp` / `.cpp` | **Boucle de contrôle** 500 Hz : mélange différentiel + anti-renversement + sécurités |
| `pid.hpp` | Régulateur **PID** réutilisable avec **anti-windup** |
| `leds.hpp` / `.cpp` · `ws2812.*` | Tâche d'état (ruban WS2812B, pilote RMT) |
| `webserver.hpp` / `.cpp` | SoftAP + serveur **HTTP/WebSocket** (commandes appairage/calibration) |
| `assets/` | `index.html` + `style.css` + `chart.min.js` — **gzippés au build** et servis en `Content-Encoding: gzip` |
| `main.cpp` | `app_main` : init des sous-systèmes + démarrage des tâches |

Tâches : **`control`** (cœur 1, 500 Hz, watchdog 5 s), **`leds`** (cœur 0, ~20 Hz) et la
**boucle BTstack** (cœur 0). Partage de `g_cfg` (mutex) et `g_status` (atomics) ;
l'état manette passe par les atomics de `input.cpp`. FreeRTOS tourne à **1000 Hz**.
Priorités / cœurs / piles : [`main/rtos.hpp`](main/rtos.hpp) · [`../doc/firmware-tasks.md`](../doc/firmware-tasks.md).

## Boucle de contrôle (500 Hz)

1. Lit la manette (`input::get()` → `x`, `y`, `connected`, `estop`, `start`).
2. Lit les **2 vitesses de roue** (chaque AS5600, dérivée d'angle 12 bits signée → km/h).
3. **Limiteur de pente** sur avance et virage (anti à-coups), puis **mélange arcade**
   `(avance y, virage x)` → consignes roue gauche / droite, après **bridage anti-renversement**.
4. Par roue : **PID de freinage** (ramène à 0 quand consigne nulle) + **PID limiteur de
   vitesse** (plafonne à `speed_limit_kmh`), sortie **plafonnée** (`duty_cap_frac`).
5. **PWM + DIR indépendants** vers les 2 canaux du driver.

`can_drive` exige : manette **connectée**, **calibrée**, **armée**, pas d'arrêt d'urgence,
pas de défaut. Sinon → **freinage des deux roues** (état **par défaut**, dès le boot). La page
web affiche un **bandeau listant clairement tous les motifs de blocage** (déconnectée, non
armée, non calibrée, e-stop, LVC, défaut capteur).

Sécurités : **armement** par appui ~1 s sur START — **bouton physique OU bouton START/Options
de la manette** (manette centrée + connectée requises ; démarrage **désarmé**),
**tout défaut force le désarmement** (il faut réarmer une fois résolu), **désarmement auto**
après inactivité, **arrêt d'urgence** (bouton B → frein immédiat), **coupure basse tension
(LVC)** avec hystérésis (+ coupure du latch) — **désactivée si le capteur de tension est
absent** (Vbat < 0 ⇒ on s'appuie sur le BMS, utile au banc sans ADS1115), **détection de
défaut capteur**, **watchdog 5 s**, **PWM plafonné** (moteurs 12 V / batterie 20 V).

> **Option `use_encoders` (0/1)** : à **0**, le firmware ignore les AS5600 — pas d'asservissement
> vitesse ni de frein PID (on s'appuie sur `duty_cap_frac`), et **pas de défaut « capteur
> bloqué »**. Indispensable pour **tester au banc sans encodeurs câblés** (sinon le défaut
> capteur se déclenche dès qu'on commande du PWM sans rotation mesurée).

### Anti-renversement (virage trop sec)

Un tricycle (2 roues motrices + 1 roulette) bascule facilement si on tourne trop fort ou
trop vite. Le virage est protégé sur **deux plans** :

1. **Amplitude (anti-renversement prédictif)** — la valeur de virage admissible est bornée
   pour que l'accélération latérale reste sous `a_lat_max` :

   ```
   a_lat ≈ 2·turn_gain·Vmax²·|avance|·|virage| / voie
   → virage_max = a_lat_max · voie / (2·turn_gain·Vmax² · |avance|)
   ```
   Plus on va vite, plus le virage est **automatiquement réduit**.

2. **Brusquerie (limiteur de pente / slew-rate)** — la consigne de virage ne peut pas varier
   de plus de `turn_rate` unités/s : un coup de manche instantané est **lissé** au lieu de
   provoquer un différentiel brutal. L'avance est lissée de même par `thr_ramp_per_s`.

Paramètres web : **`turn_gain`** (autorité de virage 0–1), **`a_lat_max`** (accél. latérale
max m/s²), **`turn_rate`** (douceur du virage, Δ/s), **`thr_ramp_per_s`** (douceur de l'avance).

## ⚠️ À ajuster avant la première mise en route

- **Capteurs de vitesse** : cinématique **hardcodée** dans `config.hpp` (`namespace hw`) —
  `AS5600_CPR = 4096`, `GEAR_RATIO = 1`, `WHEEL_DIAM_M = 0,3048` (roue 12″). **2 AS5600**,
  **un par bus I²C** (adresse fixe `0x36` → un seul capteur par bus). À **vérifier au banc**.
- **Manette** : appairer (onglet Manette) puis **calibrer** — obligatoire pour rouler.
- **Réglages web** : `vbat_div_ratio` (au multimètre), `speed_limit_kmh`, `duty_cap_frac`,
  `turn_gain` / `a_lat_max` (anti-renversement). Commencer **roues en l'air**, vitesse basse.
- **PID** : `vmax_*` (limiteur de vitesse) et `pid_*` (frein) par roue — pré-réglés
  (limiteur ≈ 0,15/0,14, frein ≈ 0,12/0,08/0,003), à **affiner au banc**.

> Vérifier le **sens de chaque roue** (inverser les fils moteur si besoin) et le **sens du
> différentiel** (pousser le stick à droite doit faire tourner à droite) **avant le sol**.
