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

#if CONFIG_IDF_TARGET_ESP32 && CONFIG_VFS_BASE_ON_SDIO
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "driver/sdio_slave.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp_vfs.h"
#include "vfs_instance.h"

static const char TAG[] = "SDIO_SLAVE";

#define container_of(ptr, type, member) ({      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - ((size_t) &((type *)0)->member));})

#define ESP_SDIO_BUFFER_SIZE      512
#define ESP_SDIO_BUFFER_NUM       10
#define ESP_SDIO_QUEUE_SIZE       20

typedef struct sdio_list {
    uint8_t pbuf[ESP_SDIO_BUFFER_SIZE];
    struct sdio_list* next;
    sdio_slave_buf_handle_t handle;
    uint32_t left_len;
    uint32_t pos;
} esp_driver_sdio_list_t;

typedef struct sdio_ctrl {
    xSemaphoreHandle semahandle;
    int fd;
    dev_select_notif_callback_t sdio_notif_callback;
} sdio_vfs_ctrl_t;

static esp_driver_sdio_list_t DRAM_ATTR sdio_buffer_list[ESP_SDIO_BUFFER_NUM];
static esp_driver_sdio_list_t* pHead;
static esp_driver_sdio_list_t* pTail;

static sdio_vfs_ctrl_t sdio_vfs_ctrl;

static uint32_t total_recv_len = 0;

static void esp32_sdio_slave_init(void)
{
    sdio_slave_config_t config = {
        .sending_mode       = SDIO_SLAVE_SEND_STREAM,
        .send_queue_size    = ESP_SDIO_QUEUE_SIZE,
        .recv_buffer_size   = ESP_SDIO_BUFFER_SIZE,
    };
    sdio_slave_buf_handle_t handle;

    sdio_vfs_ctrl.semahandle = xSemaphoreCreateMutex();
    esp_err_t ret = sdio_slave_initialize(&config);
    assert(ret == ESP_OK);

    for (int loop = 0; loop < ESP_SDIO_BUFFER_NUM; loop++) {
        handle = sdio_slave_recv_register_buf(sdio_buffer_list[loop].pbuf);
        assert(handle != NULL);

        ret = sdio_slave_recv_load_buf(handle);
        assert(ret == ESP_OK);
    }

    sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET | SDIO_SLAVE_HOSTINT_BIT0);

    sdio_slave_start();

    ESP_LOGI(TAG, "slave ready");
}

static void sdio_slave_trans_task(void* pvParameters)
{
    sdio_slave_buf_handle_t handle;
    size_t length = 0;
    uint8_t* ptr = NULL;

    for (;;) {
        // receive data from SDIO host
        esp_err_t ret = sdio_slave_recv(&handle, &ptr, &length, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Recv error,ret:%x", ret);
            continue;
        }

        ESP_LOGD(TAG, "receive len:%d", length);
        total_recv_len += length;

        xSemaphoreTake(sdio_vfs_ctrl.semahandle, portMAX_DELAY);
        esp_driver_sdio_list_t* p_list = container_of(ptr, esp_driver_sdio_list_t, pbuf); // get struct list pointer

        p_list->handle = handle;
        p_list->left_len = length;
        p_list->pos = 0;
        p_list->next = NULL;

        if (!pTail) {
            pTail = p_list;
            pHead = pTail;
        } else {
            pTail->next = p_list;
            pTail = p_list;
        }

        xSemaphoreGive(sdio_vfs_ctrl.semahandle);

        if (sdio_vfs_ctrl.sdio_notif_callback) {
            sdio_vfs_ctrl.sdio_notif_callback(sdio_vfs_ctrl.fd);
        }
    }
    vTaskDelete(NULL);
}

static void recv_count_task(void* pvParameters)
{

    while(1) {
        vTaskDelay(3000 / portTICK_RATE_MS);
        printf("Receive data: %d\r\n", total_recv_len);
    }
}

static int sdio_open(const char* path, int flags, int mode)
{
    // init slave driver
    esp32_sdio_slave_init();
    xTaskCreate(sdio_slave_trans_task , "sdio_slave_trans_task" , 4096 , NULL , 10 , NULL);
    xTaskCreate(recv_count_task , "recv_count_task" , 4096 , NULL , 4 , NULL);

    return VFS_DEV_SDIO_LOCAL_FD;
}

static int sdio_close(int fd)
{
    assert(fd == VFS_DEV_SDIO_LOCAL_FD);

    return 0;
}

static ssize_t sdio_read(int fd, void* data, size_t size)
{
    assert(fd == VFS_DEV_SDIO_LOCAL_FD);
    uint32_t copy_len = 0;
    uint32_t remain_len = 0;

    if (data == NULL || size == 0) {
        ESP_LOGE(TAG , "Cannot get read data address.");
        return -1;
    }
    remain_len = size;

    while (copy_len < size) {
        if (!pHead) {
            break;
        }

        esp_driver_sdio_list_t* p_list = pHead;

        if (remain_len < p_list->left_len) {
            memcpy(data, p_list->pbuf + p_list->pos, remain_len);
            p_list->pos += remain_len;
            p_list->left_len -= remain_len;
            copy_len += remain_len;
        } else {
            memcpy(data + copy_len, p_list->pbuf + p_list->pos, p_list->left_len);
            p_list->pos += p_list->left_len;
            copy_len += p_list->left_len;
            p_list->left_len = 0;
            xSemaphoreTake(sdio_vfs_ctrl.semahandle, portMAX_DELAY);
            pHead = p_list->next;
            p_list->next = NULL;

            if (!pHead) {
                pTail = NULL;
            }

            xSemaphoreGive(sdio_vfs_ctrl.semahandle);

            sdio_slave_recv_load_buf(p_list->handle);
            //sdio_used_list_count--;
        }
    }

    return copy_len;
}

static ssize_t sdio_write(int fd, const void * data, size_t size)
{
    assert(fd == VFS_DEV_SDIO_LOCAL_FD);
    esp_err_t ret;
    int32_t length = size;
    uint8_t* buffer = NULL;

    if (data == NULL  || length >= 4096) {
        ESP_LOGE(TAG , "Write data error, len:%d", length);
        return -1;
    }

    buffer = heap_caps_malloc(length, MALLOC_CAP_DMA);
    if (buffer == NULL) {
        ESP_LOGE(TAG , "Malloc send buffer fail!");
        return 0;
    }

    memcpy(buffer, data, length);

    ret = sdio_slave_transmit(buffer, length);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG , "sdio slave transmit error, ret : 0x%x\r\n", ret);
        length = 0;
    }

    free(buffer);
    return length;
}

bool esp_vfs_instance_want_write(void)
{
    if (pHead != NULL) {
        return true;
    } else {
        return false;
    }

}

void dev_set_select_notif_callback(int fd, dev_select_notif_callback_t dev_select_notif_callback)
{
    if (fd < 0 ) {
        ESP_LOGE(TAG, "Read error fd: %d", fd);
        return;
    }
    sdio_vfs_ctrl.fd = fd;
    sdio_vfs_ctrl.sdio_notif_callback = dev_select_notif_callback;
}

void vfs_register_transmit_instance(esp_vfs_t *vfs)
{
    if (vfs == NULL) {
        ESP_LOGE(TAG, "vfs handle uninited");
        return;
    }
    vfs->write = &sdio_write;
    vfs->open = &sdio_open;
    vfs->close = &sdio_close;
    vfs->read = &sdio_read;
}
#endif