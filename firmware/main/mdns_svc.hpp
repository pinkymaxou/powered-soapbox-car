// mdns_svc.hpp — mDNS/Bonjour advertising: reaching the kart by name instead of by IP.
#pragma once

void        mdnsStart();      // starts the responder (after wifiSoftAPInit + webServerStart)
const char* mdnsHostname();   // "kart" — WITHOUT the ".local" suffix
