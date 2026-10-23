#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "wifi.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_check.h"
#include "state.h"
#include "nvs.h"
#include "mdns.h"

#define MDNS_HOSTNAME "infrasound"
#define NVS_NS "net"

static const char *TAG = "wifi";

#define STA_MAX_RETRIES     5 // fall back to AP
#define AP_CHANNEL          1
#define AP_MAX_CONN         3
#define AP_PASSWORD "testing123"

static int s_retries;
static bool s_connected_once;
static bool s_ap_mode;

// ap status for ui
static atomic_bool s_ap_active;

static void ap_ssid(char *out, size_t sz) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, sz, "infrasound-%02X%02X", mac[4], mac[5]);
}

bool wifi_ap_info(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    if (!atomic_load(&s_ap_active)) {
        return false;
    }
    if (ssid) ap_ssid(ssid, ssid_sz);
    if (pass) strlcpy(pass, AP_PASSWORD, pass_sz);
    return true;
}

// mDNS for hostname
static void start_mdns(void) {
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS init failed (%s), use the IP address mode instead", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(MDNS_HOSTNAME);
    mdns_instance_name_set("Infrasound Monitor");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local", MDNS_HOSTNAME);
}

// check nvs for creds
static bool load_sta_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t s = ssid_sz;
        size_t p = pass_sz;
        bool ok = nvs_get_str(h, "ssid", ssid, &s) == ESP_OK && ssid[0] != '\0';
        if (ok && nvs_get_str(h, "pass", pass, &p) != ESP_OK) {
            pass[0] = '\0';
        }
        nvs_close(h);
        if (ok) {
            return true;
        }
    }
    if (CONFIG_MY_WIFI_SSID[0] != '\0') {
        strlcpy(ssid, CONFIG_MY_WIFI_SSID, ssid_sz);
        strlcpy(pass, CONFIG_MY_WIFI_PASSWORD, pass_sz);
        return true;
    }
    return false;
}

esp_err_t wifi_save_creds(const char *ssid, const char* pass) {
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_forget_creds(void) {
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void start_ap(const char *why) {
    if (s_ap_mode) {
        return;
    }
    s_ap_mode = true;

    wifi_config_t ap = {0};
    ap_ssid((char *)ap.ap.ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strlcpy((char *)ap.ap.password, AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_CONN;
    ap.ap.authmode = strlen(AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ESP_LOGW(TAG, "%s, starting AP '%s' -> http://192.168.4.1", why, ap.ap.ssid);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    atomic_store(&s_ap_active, true);
}


static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                state_set_wifi(false);
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED : {
                if (s_ap_mode) {
                    break;
                }
                wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
                state_set_wifi(false);
                if (!s_connected_once && ++s_retries >= STA_MAX_RETRIES) {
                    ESP_LOGW(TAG, "Disconnected, reason=%d, giving up after %d tries", d->reason, s_retries);
                    start_ap("station connect failed");
                } else {
                    ESP_LOGW(TAG, "Disconnected, reason=%d, retrying", d->reason);
                    esp_wifi_connect();
                }
                break;
            }

            case WIFI_EVENT_AP_START:
                state_set_wifi(true); 
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
                ESP_LOGI(TAG, "AP client joined " MACSTR, MAC2STR(e->mac));
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
                ESP_LOGI(TAG, "AP client left " MACSTR, MAC2STR(e->mac));
                break;
            }
            default:
                break;
            }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected_once = true;
        s_retries = 0;
        state_set_wifi(true);
    }
}

esp_err_t wifi_init_sta(void)
{
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();    // idle until AP mode actually starts

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    start_mdns();

    // Default is the access point: only try a station network if we have credentials for one.
    char ssid[33] = {0};
    char pass[65] = {0};
    if (!load_sta_creds(ssid, sizeof ssid, pass, sizeof pass)) {
        start_ap("no saved network");
        return esp_wifi_start();           // AP_START event marks the link up
    }

    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    return esp_wifi_start();
}
 