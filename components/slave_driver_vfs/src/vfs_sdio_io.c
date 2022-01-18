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
#include "sdkconfig.h"

#if CONFIG_VFS_BASE_ON_SDIO
#include <stdio.h>
#include "esp_system.h"
#include "esp_vfs.h"
#include "esp_vfs_dev.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "soc/uart_periph.h"

#include "vfs_instance.h"

#if CONFIG_IDF_TARGET_ESP8266
#define vfs_driver_lock()       portENTER_CRITICAL()
#define vfs_driver_unlock()     portEXIT_CRITICAL()
#else
portMUX_TYPE vfs_spinlock = portMUX_INITIALIZER_UNLOCKED;
#define vfs_driver_lock()      portENTER_CRITICAL(&vfs_spinlock)
#define vfs_driver_unlock()    portEXIT_CRITICAL(&vfs_spinlock)
#endif

static fd_set* _readfds = NULL;
static fd_set _readfds_set;
esp_vfs_select_sem_t* _signal_sem = NULL;


static void dev_select_notif_callback(int current_fd)
{
    vfs_driver_lock();
    if (_signal_sem) {
        FD_SET(current_fd, _readfds);
        esp_vfs_select_triggered(*_signal_sem);
    }
    vfs_driver_unlock();
}

static esp_err_t sdio_start_select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
        esp_vfs_select_sem_t signal_sem, void **end_select_args)
{
#ifdef CONFIG_ESP_CONSOLE_UART
    if (nfds < SOC_UART_NUM) {
        printf("Max fd %d is smaller than URAT num %d\n", nfds, SOC_UART_NUM);
        return ESP_FAIL;
    }
#endif
    vfs_driver_lock();
    _signal_sem = (esp_vfs_select_sem_t*)malloc(sizeof(esp_vfs_select_sem_t));
    memcpy(_signal_sem, &signal_sem, sizeof(esp_vfs_select_sem_t));
    _readfds = readfds;
    _readfds_set = *readfds;
    FD_ZERO(readfds);
    FD_ZERO(writefds);
    FD_ZERO(exceptfds);

    if (FD_ISSET(VFS_DEV_SDIO_LOCAL_FD, &_readfds_set)) {
        dev_set_select_notif_callback(VFS_DEV_SDIO_LOCAL_FD, dev_select_notif_callback);
        if (esp_vfs_instance_want_write()) {
            FD_SET(VFS_DEV_SDIO_LOCAL_FD, _readfds);
            esp_vfs_select_triggered(*_signal_sem);
        }
    }
    vfs_driver_unlock();
    return ESP_OK;
}

static esp_err_t sdio_end_select(void *end_select_args)
{
    vfs_driver_lock();
    dev_set_select_notif_callback(0, NULL);
    _readfds = NULL;
    free(_signal_sem);
    _signal_sem = NULL;
    vfs_driver_unlock();
    return ESP_OK;
}

void esp_vfs_dev_sdio_register(void)
{
    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .start_select = &sdio_start_select,
        .end_select = &sdio_end_select
    };
    vfs_register_transmit_instance(&vfs);
    ESP_ERROR_CHECK(esp_vfs_register(CONFIG_VFS_BASE_PATH, &vfs, NULL));
}
#endif