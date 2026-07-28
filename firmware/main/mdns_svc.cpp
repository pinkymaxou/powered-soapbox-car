// mdns_svc.cpp — mDNS (Bonjour / Avahi) responder: the kart answers to a NAME.
//
// Without mDNS you have to know the address: 192.168.4.1 on the "Kart-Config" access
// point, and whatever the router's DHCP hands out in station mode (which changes).
// With mDNS the SAME URL works on both interfaces: http://kart.local
//
// Two things are published:
//   • the host name "kart" (A / AAAA records) → resolves kart.local
//   • the "_http._tcp" service on port 80 → the kart shows up on its own in the
//     network browsers (Finder/Bonjour, avahi-browse, "Network" in Windows Explorer).
//
// Resolution works out of the box on Windows 10+, macOS/iOS and Linux (Avahi).
// ⚠️ Android resolves .local only through the NSD API — Chrome does NOT: from a phone,
// keep using http://192.168.4.1 (which is why the IP stays displayed everywhere).
//
// Cost: one task (mDNS, priority 1) and a UDP socket per interface. mDNS is a comfort
// feature, so a failure here is logged and IGNORED — it must never keep the kart from
// booting (the web server stays reachable by IP).
#include "mdns_svc.hpp"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "mdns.h"

static const char* TAG = "mdns";

namespace
{
constexpr char HOSTNAME[] = "kart";                 // → http://kart.local
constexpr char INSTANCE[] = "Kart — configuration"; // name shown by the network browsers
constexpr uint16_t HTTP_PORT = 80;                  // httpd (HTTPD_DEFAULT_CONFIG)
} // namespace

const char* mdnsHostname()
{
    return HOSTNAME;
}

void mdnsStart()
{
    const esp_err_t init = mdns_init();
    if (ESP_OK != init)
    {
        ESP_LOGE(TAG, "mdns_init: %s — the kart stays reachable by IP only", esp_err_to_name(init));
        return;
    }
    if (ESP_OK != mdns_hostname_set(HOSTNAME) || ESP_OK != mdns_instance_name_set(INSTANCE))
    {
        ESP_LOGE(TAG, "Host name not applied");
        return;
    }

    // TXT records: purely informational (visible in a Bonjour/Avahi browser).
    const esp_app_desc_t* app = esp_app_get_description();
    const mdns_txt_item_t txt[] = {
        {"path", "/"},
        {"board", "esp32"},
        {"fw", app ? app->version : "?"},
    };
    const esp_err_t svc = mdns_service_add(nullptr, "_http", "_tcp", HTTP_PORT,
                                           const_cast<mdns_txt_item_t*>(txt),
                                           sizeof(txt) / sizeof(txt[0]));
    if (ESP_OK != svc)
    {
        ESP_LOGW(TAG, "_http._tcp not published: %s", esp_err_to_name(svc));
    }
    ESP_LOGI(TAG, "mDNS ready → http://%s.local", HOSTNAME);
}
