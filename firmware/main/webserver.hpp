// webserver.hpp — Point d'accès SoftAP + serveur web/WebSocket de configuration.
#pragma once

void wifiSoftAPInit();   // démarre le Wi-Fi en mode point d'accès (SoftAP)
void webServerStart();   // démarre le serveur HTTP/WebSocket (après wifiSoftAPInit)
