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

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/debug.h"
#include "lwip/tcp.h"

uint8_t dongle_mac[6] = {0};
esp_netif_t* dongle_netif = NULL;

static const char *TAG = "netif_dongle";

//
// Internal functions declaration referenced in io object
//
static esp_err_t netsuite_io_transmit(void *h, void *buffer, size_t len);
static esp_err_t netsuite_io_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf);
static esp_err_t netsuite_io_attach(esp_netif_t * esp_netif, void * args);

esp_err_t pkt_netif2driver(void *buffer, uint16_t len);
esp_err_t esp_netif_up(esp_netif_t *esp_netif);

/**
 * @brief IO object netif related configuration with data-path function callbacks
 * and pointer to the IO object instance (unused as this is a singleton)
 */
static const esp_netif_driver_ifconfig_t c_driver_ifconfig = {
        .driver_free_rx_buffer = NULL,
        .transmit = netsuite_io_transmit,
        .transmit_wrap = netsuite_io_transmit_wrap,
        .handle = "netsuite-io-object" // this IO object is a singleton, its handle uses as a name
};

/**
 * @brief IO object base structure used to point to internal attach function
 */
static const esp_netif_driver_base_t s_driver_base = {
        .post_attach =  netsuite_io_attach
};

/**
 * @brief Transmit function called from esp_netif to output network stack data
 *
 * Note: This API has to conform to esp-netif transmit prototype
 *
 * @param h Opaque pointer representing the io driver (unused, const string in this case)
 * @param data data buffer
 * @param length length of data to send
 *
 * @return ESP_OK on success
 */
static esp_err_t netsuite_io_transmit(void *h, void *buffer, size_t len)
{
    // send data to driver
    pkt_netif2driver(buffer, len);
    return ESP_OK;
}

/**
 * @brief Transmit wrapper that is typically used for buffer handling and optimization.
 * Here just wraps the netsuite_io_transmit().
 *
 * @note The netstack_buf could be a ref-counted network stack buffer and might be used
 * by the lower layers directly if an additional handling is practical.
 * See docs on `esp_wifi_internal_tx_by_ref()` in components/esp_wifi/include/esp_private/wifi.h
 */
static esp_err_t netsuite_io_transmit_wrap(void *h, void *buffer, size_t len, void *netstack_buf)
{
    return netsuite_io_transmit(h, buffer, len);
}

/**
 * @brief Post attach adapter for netsuite i/o
 *
 * Used to exchange internal callbacks and context between esp-netif and the I/O object.
 * In case of netsuite I/O, it only updates the driver config with internal callbacks and
 * its instance pointer (const string in this case)
 *
 * @param esp_netif handle to esp-netif object
 * @param args pointer to netsuite IO
 *
 * @return ESP_OK on success
 */
static esp_err_t netsuite_io_attach(esp_netif_t * esp_netif, void * args)
{
    ESP_ERROR_CHECK(esp_netif_set_driver_config(esp_netif, &c_driver_ifconfig));
    return ESP_OK;
}

/**
 * Created (initializes) the i/o object and returns handle ready to be attached to the esp-netif
 */
static void *netsuite_io_new(void)
{
    return (void *)&s_driver_base;
}

void esp_gateway_netif_dongle_init(void)
{
    // Netif configs
    //
    esp_netif_ip_info_t ip_info;
    esp_netif_set_ip4_addr(&ip_info.ip, 192, 168 , 4, 1);
    esp_netif_set_ip4_addr(&ip_info.gw, 192, 168 , 4, 1);
    esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255 , 255, 0);

    esp_netif_inherent_config_t netif_common_config = {
            .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_GARP | ESP_NETIF_FLAG_EVENT_IP_MODIFIED),
            .ip_info = (esp_netif_ip_info_t*)&ip_info,
            .get_ip_event = IP_EVENT_STA_GOT_IP,
            .lost_ip_event = IP_EVENT_STA_LOST_IP,
            .if_key = "Virtual_key",
            .if_desc = "Netif"
    };

    esp_netif_config_t config = {
        .base = &netif_common_config,                 // use specific behaviour configuration
        .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP,      // use default WIFI-like network stack configuration
    };

    // Netif creation and configuration
    //
    // esp_netif_config_t cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t* netif = esp_netif_new(&config);
    assert(netif);
    dongle_netif = netif;
    esp_netif_attach(netif, netsuite_io_new());

    // Start the netif in a manual way, no need for events
    //
    esp_read_mac(dongle_mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "Station MAC "MACSTR"", MAC2STR(dongle_mac));
    /* Virtual Driver Netif Mac */
    dongle_mac[5] = dongle_mac[5] + 4;
    ESP_LOGI(TAG, "Driver Netif MAC "MACSTR"", MAC2STR(dongle_mac));
    esp_netif_set_mac(netif, dongle_mac);
    esp_netif_action_start(netif, NULL, 0, NULL);
    esp_netif_up(netif);

#if !CONFIG_ENABLE_SOFTAP_FOR_WIFI_CONFIG
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(114, 114, 114, 114);
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t dhcps_dns_value = OFFER_DNS;
    esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &dhcps_dns_value, sizeof(dhcps_dns_value));
    esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);

    esp_netif_dhcps_start(netif);
#endif
}
