# Test écran — ESP32-S3 + LCD tactile RGB 4,3″ (800×480)

Firmware de **test** (ESP-IDF 6.1) pour valider la dalle d'une carte de développement
**ESP32-S3** à écran tactile RGB 4,3″ : il affiche un **carré rouge en bas-gauche** sur fond
noir, via **LVGL**. Sert de base aux futurs écrans (tableau de bord, etc.).

> Projet **indépendant** du firmware du kart (`firmware/`), cible **ESP32-S3** (≠ ESP32).
> Brochage et hypothèses matérielles : [`doc/HARDWARE.md`](doc/HARDWARE.md).

## Ce que fait le firmware

1. Bus I²C (GPIO 8/9) → **CH422G** : rétroéclairage + relâche des resets + EXIO4 (SD) + EXIO5 (CAN).
2. **Panneau RGB** 16 bits 800×480 (esp_lcd) en PSRAM (double FB + gros bounce buffer),
   **pclk 30 MHz ≈ 73 Hz** (poussé pour le rafraîchissement max — voir `main.c`).
3. **LVGL** (esp_lvgl_port) : **tableau de bord véhicule** (maquette, `ui_dash.c`) — compteur
   de vitesse à aiguille, puissance moteurs G/D (barres bipolaires ±100 %), pavé joystick
   avance/virage, sélecteur PARK/DRIVE/BRAKE, barre d'état (horloge NTP, batterie). Valeurs de
   démo animées à ~5 Hz. ⚠️ Animer plus vite reglitche (écriture PSRAM ↔ scanout RGB) — voir
   `CONFIG_SPIRAM_XIP_FROM_PSRAM` dans sdkconfig.defaults.
4. **Wi-Fi AP+STA** + **page web unique** (`http://192.168.4.1`) : SSID / mot de passe /
   **case d'activation station** (persistés en NVS). **NTP** une fois connecté (fuseau Est).
5. **Carte SD** (SDMMC 1 bit) montée sur `/sdcard`. **Bus CAN** (TWAI) **1 Mbit/s, IDs étendus**.

## Compilation / flash

```bash
. ~/esp/esp-idf-6.1/export.sh
cd test/esp32s3-lcd43
idf.py set-target esp32s3      # déjà fait une fois
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # S3 = USB natif → souvent ttyACM0
```

Dépendances managées (téléchargées au build) : `lvgl/lvgl`, `espressif/esp_lvgl_port`.
Le **CH422G** est piloté directement en I²C dans [`main/main.c`](main/main.c) (pas de composant
dédié disponible sur IDF 6.1).

## À vérifier sur la carte (1er flash)

- **Écran noir + rétroéclairage éteint** → le CH422G ne répond pas : vérifier l'adresse/les
  EXIO (faire un scan I²C ; voir `doc/HARDWARE.md`).
- **Rétroéclairage allumé mais rien d'affiché (blanc/noir)** → la dalle exige probablement une
  **séquence d'init SPI (type ST7701)** : à ajouter (voir note dans `doc/HARDWARE.md`).
- **Image présente mais couleurs inversées / décalées** → ajuster l'ordre R/B ou les timings RGB
  (pclk, porches) dans `rgb_panel_init()`.
- **Carré pas dans le bon coin / image miroir** → ajuster `mirror_x/mirror_y` (rotation).

Valeurs RGB (pclk 16 MHz, porches 8/8/4) et mapping EXIO du CH422G sont des **points de départ
à confirmer au banc**.
