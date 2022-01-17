// Copyright 2015-2020 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <sys/select.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#include "esp_vfs_dev_bus.h"
#endif

#if CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
#include "tinyusb.h"
#include "tusb_net.h"
#endif

static const char* TAG = "DRIVER_ADAPTER";

#if CONFIG_WIFI_DONGLE_SPI
#define MAX_LENGTH 2048
extern esp_netif_t* dongle_netif;
static char readbuf[MAX_LENGTH];
static int fd = -1;

static void IRAM_ATTR device_recv_task(void* arg)
{
    int s;
    int recv_len = 0;
    fd_set rfds;
    struct timeval tv = {
        .tv_sec = 5,
        .tv_usec = 0,
    };

    while(1){
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        memset(readbuf, 0x0, MAX_LENGTH);
        s = select(fd + 1, &rfds, NULL, NULL, &tv);

        if (s < 0) {
            ESP_LOGE(TAG, "Select failed: errno %d", errno);
            break;
        } else if (s == 0) {
            ESP_LOGD(TAG, "Timeout has been reached and nothing has been received");
        } else {

            if (FD_ISSET(fd, &rfds)) {
                recv_len = read(fd, readbuf, MAX_LENGTH);
                if (recv_len > 0) {
                    // ESP_LOG_BUFFER_HEXDUMP(" spi ==> netif", readbuf, recv_len, ESP_LOG_INFO);
                    esp_netif_receive(dongle_netif, readbuf, recv_len, NULL);
                    ESP_LOGD(TAG, "Received len %d, data: %s", recv_len, readbuf);
                } else {
                    ESP_LOGE(TAG, "SDIO read error");
                    break;
                }
            } else {
                ESP_LOGE(TAG, "No FD has been set in select()");
                break;
            }
        }
    }
    close(fd);
}


esp_err_t pkt_netif2driver(void *buffer, uint16_t len)
{
    // ESP_LOG_BUFFER_HEXDUMP(" netif ==> spi", buffer, len, ESP_LOG_INFO);
    uint32_t length = write(fd, buffer, len);
    return ESP_OK;
}
#endif

void esp_driver_init(void)
{
#if CONFIG_WIFI_DONGLE_USB
    tusb_net_init();
    ESP_LOGI(TAG, "USB initialization");

    tinyusb_config_t tusb_cfg = {
        .external_phy = false // In the most cases you need to use a `false` value
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "USB initialization DONE");
#elif CONFIG_WIFI_DONGLE_SPI
#if CONFIG_VFS_BASE_ON_SDIO
    esp_vfs_dev_sdio_register();
    while ((fd = open("/dev/sdio", O_RDWR)) == -1) {
#else
    esp_vfs_dev_spi_register();
    while ((fd = open("/dev/spi_io/1", O_RDWR)) == -1) {
#endif
        ESP_LOGE(TAG, "Cannot open device");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
    ESP_LOGI(TAG, "Device fd : %d", fd);

    xTaskCreate(device_recv_task, "device_recv_task", 1024 * 8, NULL, 6, NULL);
#endif
}
