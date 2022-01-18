// Copyright 2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_dev.h"
#include "driver/uart.h"
#include "linenoise/linenoise.h"
#include "argtable3/argtable3.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/event_groups.h"
#include "esp_wifi.h"

#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/lwip_napt.h"

#include "esp_gateway_vendor_ie.h"
#include "esp_gateway_config.h"
#include "esp_gateway_wifi.h"
#include "esp_utils.h"

#define GATEWAY_EVENT_STA_CONNECTED  BIT0

static bool s_wifi_is_connected = false;
static EventGroupHandle_t s_wifi_event_group = NULL;
static const char *TAG = "gateway_wifi";
#if SET_VENDOR_IE
extern vendor_ie_data_t *esp_gateway_vendor_ie;
extern ap_router_t *ap_router;
extern bool first_vendor_ie_tag;
extern feat_type_t g_feat_type;
#endif

/* Event handler for catching system events */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
#if SET_VENDOR_IE
        if (g_feat_type == FEAT_TYPE_WIFI) {
            ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
            (*esp_gateway_vendor_ie).payload[CONNECT_ROUTER_STATUS] = 1;
            ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
        }
#endif
        /* Signal main application to continue execution */
        s_wifi_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, GATEWAY_EVENT_STA_CONNECTED);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGE(TAG, "Disconnected. Connecting to the AP again...");
#if SET_VENDOR_IE
        if (g_feat_type == FEAT_TYPE_WIFI) {
            if ((*esp_gateway_vendor_ie).payload[LEVEL] != 1) {
                first_vendor_ie_tag = true;
                for (int i = 0; i < 2; i++) {
                    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
                }
                ESP_ERROR_CHECK(esp_wifi_scan_stop());

                if (ap_router->level != WIFI_ROUTER_LEVEL_0) {
                    ESP_LOGI(TAG, "wifi_router_level: %d", ap_router->level);
                    esp_gateway_wifi_set(WIFI_MODE_STA, ESP_GATEWAY_WIFI_ROUTER_AP_SSID, ESP_GATEWAY_WIFI_ROUTER_AP_PASSWORD, ap_router->router_mac);
                } else {
                    ESP_LOGI(TAG, "wifi_router_level: %d", ap_router->level);
                    esp_gateway_wifi_set(WIFI_MODE_STA, ESP_GATEWAY_WIFI_ROUTER_STA_SSID, ESP_GATEWAY_WIFI_ROUTER_STA_PASSWORD, NULL);
                }

                ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
                (*esp_gateway_vendor_ie).payload[CONNECT_ROUTER_STATUS] = 0;
                (*esp_gateway_vendor_ie).payload[ROUTER_RSSI] = ap_router->rssi;
                (*esp_gateway_vendor_ie).payload[LEVEL] = ap_router->level + 1;
                ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
            } else {
                ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
                (*esp_gateway_vendor_ie).payload[CONNECT_ROUTER_STATUS] = 0;
                ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
            }
        }
#endif
        esp_wifi_connect();
        s_wifi_is_connected = false;
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGE(TAG, "STA Connecting to the AP again...");
#if SET_VENDOR_IE
        if (g_feat_type == FEAT_TYPE_WIFI) {
            ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
            (*esp_gateway_vendor_ie).payload[STATION_NUMBER]++;
            ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
        }
#endif
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGE(TAG, "STA Disconnect to the AP");
#if SET_VENDOR_IE
        if (g_feat_type == FEAT_TYPE_WIFI) {
            ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(false, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
            (*esp_gateway_vendor_ie).payload[STATION_NUMBER]--;
            ESP_ERROR_CHECK(esp_wifi_set_vendor_ie(true, WIFI_VND_IE_TYPE_BEACON, WIFI_VND_IE_ID_0, esp_gateway_vendor_ie));
        }
#endif
    }
}

esp_err_t esp_gateway_wifi_ap_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_GATEWAY_ETH_AP_SSID,
            .ssid_len = strlen(ESP_GATEWAY_ETH_AP_PASSWORD),
            .password = ESP_GATEWAY_ETH_AP_PASSWORD,
            .max_connection = ESP_GATEWAY_ETH_ROUTER_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
            .channel = ESP_GATEWAY_ETH_ROUTER_WIFI_CHANNEL
        },
    };
    if (strlen(CONFIG_ETH_ROUTER_WIFI_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));

    return ESP_OK;
}

esp_netif_t *esp_gateway_wifi_init(wifi_mode_t mode)
{
    if (s_wifi_event_group) {
        return NULL;
    }

    esp_netif_t *wifi_netif = NULL;

    s_wifi_event_group = xEventGroupCreate();

    if (mode & WIFI_MODE_STA) {
        wifi_netif = esp_netif_create_default_wifi_sta();
    }

    if (mode & WIFI_MODE_AP) {
        wifi_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register our event handler for Wi-Fi, IP and Provisioning related events */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(mode));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());

    return wifi_netif;
}

esp_err_t esp_gateway_wifi_set(wifi_mode_t mode, const char *ssid, const char *password, const char *bssid)
{
    ESP_PARAM_CHECK(ssid);
    ESP_PARAM_CHECK(password);

    wifi_config_t wifi_cfg = {0};

    if (mode & WIFI_MODE_STA) {
        strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
        strlcpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password));
        if (bssid != NULL) {
            wifi_cfg.sta.bssid_set = 1;
            memcpy((char *)wifi_cfg.sta.bssid, bssid, sizeof(wifi_cfg.sta.bssid));
            ESP_LOGI(TAG, "sta ssid: %s password: %s MAC "MACSTR"", ssid, password, MAC2STR(wifi_cfg.sta.bssid));
        } else {
            ESP_LOGI(TAG, "sta ssid: %s password: %s", ssid, password);
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_cfg));   
    }

    if (mode & WIFI_MODE_AP) {
        wifi_cfg.ap.max_connection = 10;
        wifi_cfg.ap.authmode = strlen(password) < 8 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        strlcpy((char *)wifi_cfg.ap.ssid, ssid, sizeof(wifi_cfg.ap.ssid));
        strlcpy((char *)wifi_cfg.ap.password, password, sizeof(wifi_cfg.ap.password));

        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_cfg));

        ESP_LOGI(TAG, "softap ssid: %s password: %s", ssid, password);
    }

    return ESP_OK;
}

esp_err_t esp_gateway_wifi_napt_enable(uint32_t ip)
{
#ifdef ESP_GATEWAY_AP_CUSTOM_IP
    esp_netif_ip_info_t info_t;
    memset(&info_t, 0, sizeof(esp_netif_ip_info_t));
    ip4addr_aton((const char *)(ESP_GATEWAY_AP_STATIC_IP_ADDR), (ip4_addr_t*)&info_t.ip);
    ip_napt_enable(info_t.ip.addr, 1);
#else
    ip_napt_enable(ip, 1);
#endif
    ESP_LOGI(TAG, "NAT is enabled");

    return ESP_OK;
}

esp_err_t esp_gateway_wifi_set_dhcps(esp_netif_t *netif, uint32_t addr)
{
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = addr;
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    return ESP_OK;
}

esp_err_t esp_gateway_set_custom_ip_network_segment(esp_netif_t *netif, char *ip, char *gateway, char *netmask)
{
    ESP_PARAM_CHECK(netif);

    esp_netif_ip_info_t info_t;
    memset(&info_t, 0, sizeof(esp_netif_ip_info_t));

    ESP_ERROR_CHECK(esp_netif_dhcps_stop(netif));

    ip4addr_aton((const char *)ip, (ip4_addr_t*)&info_t.ip);
    ip4addr_aton((const char *)netmask, (ip4_addr_t*)&info_t.netmask);
    ip4addr_aton((const char *)gateway, (ip4_addr_t*)&info_t.gw);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &info_t));

    ESP_ERROR_CHECK(esp_netif_dhcps_start(netif));

    return ESP_OK;
}

esp_err_t esp_gateway_wifi_sta_connected(uint32_t wait_ms)
{
    esp_wifi_connect();
    xEventGroupWaitBits(s_wifi_event_group, GATEWAY_EVENT_STA_CONNECTED,
                        true, true, pdMS_TO_TICKS(wait_ms));
    return ESP_OK;
}

bool esp_gateway_wifi_is_connected()
{
    return s_wifi_is_connected;
}