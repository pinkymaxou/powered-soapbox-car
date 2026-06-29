# Carte de test — ESP32-S3 + écran tactile RGB 4,3″ (800×480)

Notes matérielles pour le firmware de test (`test/esp32s3-lcd43`). Désignations par
**numéro de composant** (génériques), sans nom de fournisseur.

## Vue d'ensemble

| Élément | Détail |
|---|---|
| MCU | **ESP32-S3** (Xtensa LX7 double cœur, 240 MHz, Wi-Fi/BT) |
| Flash | **16 Mo** |
| PSRAM | **Octal** (nécessaire pour le framebuffer RGB) |
| Écran | LCD **RGB 16 bits (RGB565)**, **800×480** |
| Contrôleur LCD | **ST7701** *(à confirmer : les variantes 800×480 4,3″ sont souvent des dalles RGB « simples » sans init SPI ; voir [Note ST7701](#note-st7701))* |
| Tactile | **GT911** capacitif (I²C, 5 points, INT) |
| Expandeur d'E/S | **CH422G** (I²C) — pilote reset LCD/tactile, **rétroéclairage**, CS carte SD |
| Périphériques | CAN, RS485, I²C (PH2.0), lecteur micro-SD, USB pleine vitesse |

## Brochage RGB (LCD parallèle 16 bits)

| Signal | GPIO |
|---|---|
| DE | **5** |
| VSYNC | **3** |
| HSYNC | **46** |
| PCLK | **7** |
| Données D0..D15 | **14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40** |

Ordre RGB565 sur le bus 16 bits : D0–D4 = **B0–B4**, D5–D10 = **G0–G5**, D11–D15 = **R0–R4**
(convention usuelle de ces dalles ; à vérifier si les couleurs sont inversées).

**Timings RGB** (point de départ, à affiner au banc) : pclk ≈ **16 MHz** ;
HSYNC pulse 4 / back 8 / front 8 ; VSYNC pulse 4 / back 8 / front 8.

## I²C (bus partagé tactile + expandeur)

| Signal | GPIO |
|---|---|
| SDA | **8** |
| SCL | **9** |

Adresses I²C (à confirmer par un scan — c'est le 1er test du fabricant) :
- **GT911** : `0x5D` (ou `0x14` selon l'état de la broche INT au reset).
- **CH422G** : interface à adresses multiples (commande `~0x24`, sortie `~0x38`, entrée `~0x26`).

## Expandeur CH422G (EXIO)

| EXIO | Fonction (confirmé wiki) |
|---|---|
| EXIO1 | **Reset tactile** (GT911) |
| EXIO2 | **Activation rétroéclairage** |
| EXIO3 | Reset LCD *(à confirmer)* |
| EXIO4 | **CS carte SD** |
| EXIO5 | *(à confirmer — USB/CAN sel.)* |

> ⚠️ Le rétroéclairage passe par **EXIO2** du CH422G : sans init du CH422G, l'écran reste **noir**.

## Carte SD (TF) — SDMMC 1 bit

| Signal | GPIO |
|---|---|
| CLK | **12** |
| CMD (MOSI) | **11** |
| D0 (MISO) | **13** |
| Enable / CS | **EXIO4** du CH422G (actif bas) |

Utilisée en **SDMMC 1 bit** (pas de CS dans le protocole). EXIO4 mis à **bas** à l'init.
Si le montage échoue (`TIMEOUT`) : vérifier qu'une carte est insérée, la polarité d'EXIO4,
et la présence de pull-ups sur CMD/D0.

## Bus CAN — TWAI

| Signal | GPIO |
|---|---|
| TX | **15** |
| RX | **16** |
| Mode transceiver | **EXIO5** du CH422G : **haut = CAN** (bas = USB) |

Configuré **1 Mbit/s, identifiants étendus (29 bits)**, filtre accept-all. Émet une trame
étendue de test (`0x12345678`) périodiquement. Si rien n'est reçu/émis sur l'analyseur :
**intervertir TX/RX (15↔16)** et vérifier EXIO5 (mode CAN) + la résistance de terminaison.

## Note ST7701

800×480 n'est pas une résolution native du ST7701 (480×864). Les cartes 4,3″ 800×480 de ce
format sont en général des **dalles RGB « simples »** (aucune séquence d'init SPI) : on configure
seulement le panneau RGB + les timings. Le firmware de test part de cette hypothèse. **Si l'écran
reste blanc/noir** alors que le rétroéclairage est allumé, c'est qu'une **séquence d'init SPI
(type ST7701, 3 fils)** est requise → à ajouter à ce moment-là.

## Sources techniques

Brochage recoupé depuis le wiki du fabricant et le composant ESP-IDF communautaire
(registre de composants Espressif). Datasheets des composants (GT911, CH422G, ST7701) à
déposer dans ce dossier `doc/`.
