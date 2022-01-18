// Copyright 2020-2021 Espressif Systems (Shanghai) PTE LTD
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

#pragma once

#include <esp_err.h>
#include "esp_wifi.h"

#include "lwip/opt.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "lwip/lwip_napt.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define MAC2STR(a)                  (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define MACSTR                      "%02x:%02x:%02x:%02x:%02x:%02x"

esp_err_t esp_gateway_wifi_ap_init(void);
esp_netif_t *esp_gateway_wifi_init(wifi_mode_t mode);
esp_err_t esp_gateway_wifi_set(wifi_mode_t mode, const char *ssid, const char *password, const char *bssid);
esp_err_t esp_gateway_wifi_napt_enable(uint32_t ip);
bool esp_gateway_wifi_is_connected();
esp_err_t esp_gateway_wifi_sta_connected(uint32_t wait_ms);
esp_err_t esp_gateway_set_custom_ip_network_segment(esp_netif_t *netif, char *ip, char *gateway, char *netmask);
esp_err_t esp_gateway_wifi_set_dhcps(esp_netif_t *netif, uint32_t addr);

#ifdef __cplusplus
}
#endif