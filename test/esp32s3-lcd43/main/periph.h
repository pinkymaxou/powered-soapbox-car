// periph.h — Périphériques de test : carte SD (SDMMC 1-bit) et bus CAN (TWAI 1 Mbit/s).
#pragma once

void sd_init(void);    // monte la carte SD sur /sdcard (SDMMC 1-bit). Non fatal si absente.
void can_init(void);   // démarre le CAN 1 Mbit/s (IDs étendus) + tâche RX/TX de test.
