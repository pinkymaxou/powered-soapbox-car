// wifi_web.h — Petit point d'accès + page web unique pour configurer le Wi-Fi station.
#pragma once

// Démarre Wi-Fi (AP+STA), charge les identifiants enregistrés et lance le serveur web.
// Point d'accès « LCD-Test » → http://192.168.4.1 pour saisir SSID/mot de passe.
void wifi_web_start(void);
