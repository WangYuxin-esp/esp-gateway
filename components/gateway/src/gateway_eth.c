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
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_private/wifi.h"
#include "netif/ethernet.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

static const char *TAG                 = "gateway_eth";
static esp_eth_handle_t s_eth_handle   = NULL;
static xQueueHandle flow_control_queue = NULL;
static bool s_wifi_is_connected        = false;
static bool s_wifi_is_started          = false;
static bool s_ethernet_is_connected    = false;
static wifi_mode_t g_wifi_mode     = WIFI_MODE_AP;

#define FLOW_CONTROL_QUEUE_TIMEOUT_MS (200)
#define FLOW_CONTROL_QUEUE_LENGTH (50)
#define FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS (100)

extern esp_netif_t* virtual_netif;
extern uint8_t virtual_mac[];
const uint8_t ipv4_multicast[3] = {0x01, 0x00, 0x5e};
const uint8_t ipv6_multicast[3] = {0x33, 0x33};
const uint8_t ip_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

typedef struct {
    void *packet;
    uint16_t length;
} flow_control_msg_t;

// Forward packets from Wi-Fi to Ethernet
static esp_err_t pkt_wifi2eth(void *buffer, uint16_t len, void *eb)
{
    if (g_wifi_mode == WIFI_MODE_AP) {
        struct eth_hdr* eth_header = NULL;
        eth_header = buffer;
        if ((memcmp(virtual_mac, eth_header->dest.addr, 6) == 0)
            || (memcmp(ipv4_multicast, eth_header->dest.addr, sizeof(ipv4_multicast)) == 0)
            || (memcmp(ipv6_multicast, eth_header->dest.addr, sizeof(ipv6_multicast)) == 0)
            || (memcmp(ip_broadcast, eth_header->dest.addr, sizeof(ip_broadcast)) == 0)) {
                void* data = malloc(len);
                assert(data);
                memcpy(data, buffer, len);
                esp_netif_receive(virtual_netif, data, len, NULL);
        }
    }

    if (s_ethernet_is_connected) {
        if (esp_eth_transmit(s_eth_handle, buffer, len) != ESP_OK) {
            ESP_LOGE(TAG, "Ethernet send packet failed");
        }
    }

    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

esp_err_t pkt_virnet2eth(void *buffer, uint16_t len)
{
    if (s_wifi_is_connected) {
        esp_wifi_internal_tx(g_wifi_mode - 1, buffer, len);
    }

    if (s_ethernet_is_connected) {
        if (esp_eth_transmit(s_eth_handle, buffer, len) != ESP_OK) {
            ESP_LOGE(TAG, "Ethernet send packet failed");
        }
    }
    return ESP_OK;
}

// Forward packets from Ethernet to Wi-Fi
// Note that, Ethernet works faster than Wi-Fi on ESP32,
// so we need to add an extra queue to balance their speed difference.
static esp_err_t pkt_eth2wifi(esp_eth_handle_t eth_handle, uint8_t *buffer, uint32_t len, void *priv)
{
    esp_err_t ret = ESP_OK;
    flow_control_msg_t msg = {
        .packet = buffer,
        .length = len
    };

    if (g_wifi_mode == WIFI_MODE_STA) {
        if (xQueueSend(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "send flow control message failed or timeout, free_heap: %d", esp_get_free_heap_size());
            free(buffer);
            ret = ESP_FAIL;
        }
    } else if (g_wifi_mode == WIFI_MODE_AP) {
        struct eth_hdr* eth_header = NULL;
        bool stack_run = false;
        eth_header = msg.packet;
        if ((memcmp(virtual_mac, eth_header->dest.addr, 6) == 0)
            || (memcmp(ipv4_multicast, eth_header->dest.addr, sizeof(ipv4_multicast)) == 0)
            || (memcmp(ipv6_multicast, eth_header->dest.addr, sizeof(ipv6_multicast)) == 0)
            || (memcmp(ip_broadcast, eth_header->dest.addr, sizeof(ip_broadcast)) == 0)) {
            esp_netif_receive(virtual_netif, msg.packet, msg.length, NULL);
            stack_run = true;
        }
        if (s_wifi_is_connected) {
            esp_wifi_internal_tx(ESP_IF_WIFI_AP, msg.packet, msg.length);
        }
        if (!stack_run) {
            free(msg.packet);
        }
    }

    return ret;
}

// This task will fetch the packet from the queue, and then send out through Wi-Fi.
// Wi-Fi handles packets slower than Ethernet, we might add some delay between each transmitting.
static void eth2wifi_flow_control_task(void *args)
{
    flow_control_msg_t msg;
    int res = 0;
    uint32_t timeout = 0;

    while (1) {
        if (xQueueReceive(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) == pdTRUE) {
            timeout = 0;
            ESP_LOGD(TAG, "[%s, %d], connected: %d, length: %d, dest_mac: " MACSTR ", src_mac: " MACSTR", " MACSTR,
                     __func__, __LINE__, s_wifi_is_connected, msg.length,
                     MAC2STR((uint8_t *)msg.packet), MAC2STR((uint8_t *)msg.packet + 6), MAC2STR((uint8_t *)msg.packet + 12));

            if (g_wifi_mode == WIFI_MODE_STA && !s_wifi_is_connected) {
                uint8_t pc_mac[6] ={0};
                uint8_t sta_mac[6] ={0};
                memcpy(pc_mac, msg.packet + 6, 6);
                esp_wifi_get_mac(WIFI_IF_STA, sta_mac);

                ESP_LOGI(TAG, "set STA MAC: " MACSTR", pc_mac: " MACSTR, MAC2STR(sta_mac), MAC2STR(pc_mac));

                if(memcmp(sta_mac, pc_mac, 6) || !s_wifi_is_started) {
                    s_wifi_is_started = true;
                    esp_wifi_start();
                    esp_wifi_set_mac(WIFI_IF_STA, (uint8_t *)msg.packet + 6);
                    esp_wifi_connect();
                }
            }

            if (s_wifi_is_connected && msg.length) {
                do {
                    res = esp_wifi_internal_tx(ESP_IF_WIFI_STA, msg.packet, msg.length);
                    vTaskDelay(pdMS_TO_TICKS(timeout));
                    timeout += 5;
                } while (res && timeout < FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS);

                if (res != ESP_OK) {
                    ESP_LOGE(TAG, "<%s> WiFi send packet failed: %d", esp_err_to_name(res), res);
                }
            }

            free(msg.packet);
        }
    }

    vTaskDelete(NULL);
}

// Event handler for Ethernet
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            s_ethernet_is_connected = true;

            if(g_wifi_mode == WIFI_MODE_AP){
                uint8_t eth_mac[6] = {0};
                esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, eth_mac);
                esp_wifi_set_mac(WIFI_MODE_AP, eth_mac);
                ESP_LOGI(TAG, "eth_mac: " MACSTR, MAC2STR(eth_mac));
                ESP_ERROR_CHECK(esp_wifi_start());
            }
            if (virtual_netif != NULL) {
                esp_netif_action_start(virtual_netif, NULL, 0, NULL);
                esp_netif_dhcpc_start(virtual_netif);
            }
            break;

        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            s_ethernet_is_connected = false;
            s_wifi_is_started = false;
            ESP_ERROR_CHECK(esp_wifi_stop());
            esp_netif_dhcpc_stop(virtual_netif);
            esp_netif_action_stop(virtual_netif, NULL, 0, NULL);
            break;

        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;

        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;

        default:
            break;
    }
}

// Event handler for Wi-Fi
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static uint8_t s_con_cnt = 0;

    switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "Wi-Fi AP got a station connected");

            if (!s_con_cnt) {
                s_wifi_is_connected = true;
                s_wifi_is_started = true;
                esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_AP, pkt_wifi2eth);
                wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
                ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",MAC2STR(event->mac), event->aid);
            }

            s_con_cnt++;
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "Wi-Fi AP got a station disconnected");
            s_con_cnt--;

            if (!s_con_cnt) {
                s_wifi_is_connected = false;
                esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_AP, NULL);
            }

            break;

        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "Wi-Fi STA connected");
            s_wifi_is_started = true;
            s_wifi_is_connected = true;

            esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, pkt_wifi2eth);
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "Wi-Fi STA disconnected");
            s_wifi_is_connected = false;
            esp_wifi_internal_reg_rxcb(ESP_IF_WIFI_STA, NULL);

            if(s_ethernet_is_connected) {
                esp_wifi_connect();
            }
            break;

        default:
            break;
    }
}

static esp_err_t initialize_flow_control(void)
{
    flow_control_queue = xQueueCreate(FLOW_CONTROL_QUEUE_LENGTH, sizeof(flow_control_msg_t));

    if (!flow_control_queue) {
        ESP_LOGE(TAG, "create flow control queue failed");
        return ESP_FAIL;
    }

    BaseType_t ret = xTaskCreate(eth2wifi_flow_control_task, "flow_ctl", 2048, NULL, (tskIDLE_PRIORITY + 5), NULL);

    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "create flow control task failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t esp_gateway_eth_init(void)
{
    esp_wifi_get_mode(&g_wifi_mode);
    ESP_LOGI(TAG, "wifi mode: %d", g_wifi_mode);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    if (g_wifi_mode == WIFI_MODE_STA) {
        ESP_ERROR_CHECK(initialize_flow_control());
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = CONFIG_EXAMPLE_ETH_PHY_RST_GPIO;
#if CONFIG_EXAMPLE_USE_INTERNAL_ETHERNET
    mac_config.smi_mdc_gpio_num = CONFIG_EXAMPLE_ETH_MDC_GPIO;
    mac_config.smi_mdio_gpio_num = CONFIG_EXAMPLE_ETH_MDIO_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);
#if CONFIG_EXAMPLE_ETH_PHY_IP101
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#elif CONFIG_EXAMPLE_ETH_PHY_RTL8201
    esp_eth_phy_t *phy = esp_eth_phy_new_rtl8201(&phy_config);
#elif CONFIG_EXAMPLE_ETH_PHY_LAN8720
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 4, 0)
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
#else
    esp_eth_phy_t *phy = esp_eth_phy_new_lan8720(&phy_config);
#endif //ESP_IDF_VERSION
#elif CONFIG_EXAMPLE_ETH_PHY_DP83848
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
#endif
#elif CONFIG_EXAMPLE_USE_DM9051
    gpio_install_isr_service(0);
    spi_device_handle_t spi_handle = NULL;
    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_EXAMPLE_DM9051_MISO_GPIO,
        .mosi_io_num = CONFIG_EXAMPLE_DM9051_MOSI_GPIO,
        .sclk_io_num = CONFIG_EXAMPLE_DM9051_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(CONFIG_EXAMPLE_DM9051_SPI_HOST, &buscfg, 1));
    spi_device_interface_config_t devcfg = {
        .command_bits = 1,
        .address_bits = 7,
        .mode = 0,
        .clock_speed_hz = CONFIG_EXAMPLE_DM9051_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_EXAMPLE_DM9051_CS_GPIO,
        .queue_size = 20
    };
    ESP_ERROR_CHECK(spi_bus_add_device(CONFIG_EXAMPLE_DM9051_SPI_HOST, &devcfg, &spi_handle));
    /* dm9051 ethernet driver is based on spi driver */
    eth_dm9051_config_t dm9051_config = ETH_DM9051_DEFAULT_CONFIG(spi_handle);
    dm9051_config.int_gpio_num = CONFIG_EXAMPLE_DM9051_INT_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_dm9051(&dm9051_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_dm9051(&phy_config);
#endif
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    memcpy(virtual_mac, mac, 6);
    config.stack_input = pkt_eth2wifi;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &s_eth_handle));
    bool eth_promiscuous = true;
    esp_eth_ioctl(s_eth_handle, ETH_CMD_S_PROMISCUOUS, &eth_promiscuous);
    esp_eth_start(s_eth_handle);

    return ESP_OK;
}
