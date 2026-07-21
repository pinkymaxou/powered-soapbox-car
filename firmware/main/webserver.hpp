// webserver.hpp — SoftAP access point + configuration web/WebSocket server.
#pragma once

void wifiSoftAPInit();   // starts Wi-Fi in access point mode (SoftAP)
void webServerStart();   // starts the HTTP/WebSocket server (after wifiSoftAPInit)
