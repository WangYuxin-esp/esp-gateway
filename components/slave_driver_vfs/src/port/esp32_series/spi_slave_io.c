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

#if !CONFIG_IDF_TARGET_ESP8266 && !CONFIG_IDF_TARGET_ESP32 && CONFIG_VFS_BASE_ON_SPI
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "driver/spi_slave_hd.h"
#include "driver/gpio.h"

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#ifdef CONFIG_SPI_STREAM_MODE
#include "freertos/stream_buffer.h"
#elif defined(CONFIG_SPI_PACKET_MODE)
#include "freertos/ringbuf.h"
#endif

#include "esp_vfs.h"
#include "vfs_instance.h"

static const char TAG[] = "SEG_SLAVE";

#define SPI_SLAVE_HANDSHARK_GPIO CONFIG_SPI_HANDSHAKE_PIN
#define GPIO_MOSI CONFIG_SPI_MOSI_PIN
#define GPIO_MISO CONFIG_SPI_MISO_PIN
#define GPIO_SCLK CONFIG_SPI_SCLK_PIN
#define GPIO_CS CONFIG_SPI_CS_PIN

#ifdef CONFIG_SPI_QUAD_MODE
#define GPIO_WP CONFIG_SPI_WP_PIN
#define GPIO_HD CONFIG_SPI_HD_PIN
#endif

#define SLAVE_HOST SPI2_HOST
#define DMA_CHAN 1

#define SPI_SLAVE_HANDSHARK_SEL      (1ULL<<SPI_SLAVE_HANDSHARK_GPIO)

#define SPI_DMA_MAX_LEN             4092
#define SPI_WRITE_STREAM_BUFFER     CONFIG_TX_STREAM_BUFFER_SIZE
#define SPI_READ_STREAM_BUFFER      CONFIG_RX_STREAM_BUFFER_SIZE
#define SLAVE_CONFIG_ADDR           4
#define MASTER_CONFIG_ADDR          0

typedef enum {
    SPI_NULL = 0,
    SPI_SLAVE_WR,         // slave -> master
    SPI_SLAVE_RD,         // maste -> slave
} spi_mode_t;
typedef struct {
    spi_mode_t direct;
} spi_msg_t;

typedef struct {
    uint32_t     direct : 8;
    uint32_t     seq_num : 8;
    uint32_t     transmit_len : 16;
} spi_rd_status_opt_t;

typedef struct {
    uint32_t     magic    : 8;
    uint32_t     send_seq : 8;
    uint32_t     send_len : 16;
} spi_wr_status_opt_t;

static uint8_t spi_slave_send_seq_num = 0;
static uint8_t spi_slave_recv_seq_num = 0;

// defined in vfs_xx_io.c
extern esp_vfs_select_sem_t* _signal_sem;
extern portMUX_TYPE vfs_spinlock;

#ifdef CONFIG_SPI_STREAM_MODE
static StreamBufferHandle_t spi_slave_rx_ring_buf = NULL;
static StreamBufferHandle_t spi_slave_tx_ring_buf = NULL;
#elif defined(CONFIG_SPI_PACKET_MODE)
static RingbufHandle_t spi_slave_rx_ring_buf = NULL;
static RingbufHandle_t spi_slave_tx_ring_buf = NULL;
static size_t ringbuffer_tx_item_size = 0;
static size_t ringbuffer_rx_item_size = 0;
#endif

static xQueueHandle msg_queue;
static uint8_t initiative_send_flag = 0;

static spi_wr_status_opt_t wr_status;

static bool send_queue_error_flag = false;

static xSemaphoreHandle pxMutex;

void spi_mutex_lock(void)
{
    while (xSemaphoreTake(pxMutex, portMAX_DELAY) != pdPASS);
}

void spi_mutex_unlock(void)
{
    xSemaphoreGive(pxMutex);
}

bool cb_master_write_buffer(void* arg, spi_slave_hd_event_t* event, BaseType_t* awoken)
{
    //Give the semaphore.
    BaseType_t mustYield = false;
    spi_msg_t spi_msg = {
        .direct = SPI_SLAVE_RD,
    };

    if (xQueueSendFromISR(msg_queue, (void*)&spi_msg, &mustYield) != pdPASS) {
        ets_printf("send queue from isr error\n");
        send_queue_error_flag = true;
    }
    return true;
}

inline static void write_transmit_len(spi_mode_t spi_mode, uint16_t transmit_len)
{
    ESP_EARLY_LOGV(TAG, "write rd status: %d, %d", (uint32_t)spi_mode, transmit_len);
    if (transmit_len > SPI_WRITE_STREAM_BUFFER) {
        ESP_EARLY_LOGI(TAG, "Set error RD len: %d", transmit_len);
        return;
    }
    spi_rd_status_opt_t rd_status_opt;
    rd_status_opt.direct = spi_mode;
    rd_status_opt.transmit_len = transmit_len;
    if (spi_mode == SPI_SLAVE_WR) {    // slave -> master
        rd_status_opt.seq_num = ++spi_slave_send_seq_num;
    } else if (spi_mode == SPI_SLAVE_RD) {                             // SPI_SLAVE_RECV   maste -> slave
        rd_status_opt.seq_num = ++spi_slave_recv_seq_num;
    }

    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_CONFIG_ADDR, (uint8_t*)&rd_status_opt, sizeof(spi_rd_status_opt_t));
}

static void spi_transmit_task(void* pvParameters)
{
    spi_slave_hd_data_t slave_trans;
    spi_slave_hd_data_t* ret_trans;
    spi_msg_t trans_msg = {0};
    uint32_t send_len = 0;
    uint32_t tmp_send_len = 0;
    uint32_t remain_len = 0;

#ifdef CONFIG_SPI_PACKET_MODE
    uint8_t* transmit_point = NULL;
#endif

    uint8_t* data_buf = (uint8_t*)malloc(SPI_DMA_MAX_LEN * sizeof(uint8_t));
    if (data_buf == NULL) {
        ESP_LOGE(TAG, "malloc fail");
        return;
    }

    while (1) {
        memset(data_buf, 0x0, SPI_DMA_MAX_LEN);
        memset(&trans_msg, 0x0, sizeof(spi_msg_t));

        xQueueReceive(msg_queue, (void*)&trans_msg, (portTickType)portMAX_DELAY);
        ESP_LOGD(TAG, "Direct: %d", trans_msg.direct);
        if (trans_msg.direct == SPI_SLAVE_RD) {    // master -> slave
            write_transmit_len(SPI_SLAVE_RD, SPI_DMA_MAX_LEN);    

            gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 0);
            
            memset(&slave_trans, 0x0, sizeof(spi_slave_hd_data_t));
            slave_trans.data = data_buf;
            slave_trans.len = SPI_DMA_MAX_LEN;
            ESP_ERROR_CHECK(spi_slave_hd_queue_trans(SLAVE_HOST, SPI_SLAVE_CHAN_RX, &slave_trans, portMAX_DELAY));

            gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 1);

            ESP_ERROR_CHECK(spi_slave_hd_get_trans_res(SLAVE_HOST, SPI_SLAVE_CHAN_RX, &ret_trans, portMAX_DELAY));
            if (ret_trans->trans_len > SPI_READ_STREAM_BUFFER || ret_trans->trans_len <= 0) {
                ESP_LOGE(TAG, "Recv error len: %d, %d, %x\n", ret_trans->trans_len, slave_trans.len, data_buf[0]);
                break;
            }

#ifdef CONFIG_SPI_STREAM_MODE
            xStreamBufferSend(spi_slave_rx_ring_buf, (void*) data_buf, ret_trans->trans_len, portMAX_DELAY);
#elif defined(CONFIG_SPI_PACKET_MODE)
            if (xRingbufferSend(spi_slave_rx_ring_buf, (void*)data_buf, ret_trans->trans_len, portMAX_DELAY) == pdFALSE) {
                ESP_LOGE(TAG, "Send len %d to buffer error, please enlarge the buffer RX size", ret_trans->trans_len);
                break;
            }
#endif
            portENTER_CRITICAL(&vfs_spinlock);
            if (_signal_sem != NULL) {
                esp_vfs_select_triggered(*_signal_sem);
            }
            portEXIT_CRITICAL(&vfs_spinlock);

        } else if (trans_msg.direct == SPI_SLAVE_WR) {     // slave -> master
#ifdef CONFIG_SPI_STREAM_MODE
            remain_len = xStreamBufferBytesAvailable(spi_slave_tx_ring_buf);
            if (remain_len > 0){
                send_len = remain_len > SPI_DMA_MAX_LEN ? SPI_DMA_MAX_LEN : remain_len;
#elif defined(CONFIG_SPI_PACKET_MODE)
            transmit_point = xRingbufferReceive(spi_slave_tx_ring_buf, (size_t*)&send_len, 0);
            if (send_len > 0 && transmit_point != NULL){
#endif
                write_transmit_len(SPI_SLAVE_WR, send_len);

            } else {
                ESP_LOGD(TAG, "Receive send queue but no data");
                initiative_send_flag = 0;
                continue;
            }

            gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 0);
            memset(&slave_trans, 0x0, sizeof(spi_slave_hd_data_t));
#ifdef CONFIG_SPI_STREAM_MODE
            tmp_send_len = xStreamBufferReceive(spi_slave_tx_ring_buf, (void*) data_buf, send_len, 0);
            if (send_len != tmp_send_len) {
                ESP_LOGE(TAG, "Read len expect %d, but actual read %d", send_len, tmp_send_len);
                break;
            }
            slave_trans.data = (uint8_t*)data_buf;
#elif defined(CONFIG_SPI_PACKET_MODE)
            slave_trans.data = (uint8_t*)transmit_point;
#endif
            slave_trans.len = send_len;
            ESP_ERROR_CHECK(spi_slave_hd_queue_trans(SLAVE_HOST, SPI_SLAVE_CHAN_TX, &slave_trans, portMAX_DELAY));
            gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 1);
            ESP_ERROR_CHECK(spi_slave_hd_get_trans_res(SLAVE_HOST, SPI_SLAVE_CHAN_TX, &ret_trans, portMAX_DELAY));

            spi_mutex_lock();
#ifdef CONFIG_SPI_STREAM_MODE
            remain_len = xStreamBufferBytesAvailable(spi_slave_tx_ring_buf);
            if (remain_len > 0){
#elif defined(CONFIG_SPI_PACKET_MODE)
            vRingbufferReturnItem(spi_slave_tx_ring_buf, transmit_point);
            vRingbufferGetInfo(spi_slave_tx_ring_buf, NULL, NULL, NULL, NULL, &ringbuffer_tx_item_size);
            if (ringbuffer_tx_item_size > 0){
#endif
                spi_msg_t spi_msg = {
                    .direct = SPI_SLAVE_WR,
                };
                if (xQueueSend(msg_queue, (void*)&spi_msg, 0) != pdPASS) {
                    ESP_LOGE(TAG, "send WR queue error");
                    spi_mutex_unlock();
                    break;
                }
            } else {
                initiative_send_flag = 0;
            }
            spi_mutex_unlock();

        } else {
            ESP_LOGE(TAG, "Unknow direct: %d", trans_msg.direct);
            continue;
        }
    }

    free(data_buf);
    vTaskDelete(NULL);

}

void spi_bus_default_config(spi_bus_config_t* bus_cfg)
{
    bus_cfg->mosi_io_num = GPIO_MOSI;
    bus_cfg->miso_io_num = GPIO_MISO;
    bus_cfg->sclk_io_num = GPIO_SCLK;
#ifdef CONFIG_SPI_QUAD_MODE
    bus_cfg->quadwp_io_num = GPIO_WP;
    bus_cfg->quadhd_io_num = GPIO_HD;
    bus_cfg->flags = SPICOMMON_BUSFLAG_QUAD;
#else
    bus_cfg->quadwp_io_num = -1;
    bus_cfg->quadhd_io_num = -1;
#endif

#ifdef CONFIG_SPI_DUAL_MODE
    bus_cfg->flags = SPICOMMON_BUSFLAG_DUAL;
#endif
    bus_cfg->max_transfer_sz = 14000;
}

void spi_slot_default_config(spi_slave_hd_slot_config_t* slave_hd_cfg)
{
    slave_hd_cfg->spics_io_num = GPIO_CS;
    slave_hd_cfg->flags = 0;
    slave_hd_cfg->mode = 0;
    slave_hd_cfg->command_bits = 8;
    slave_hd_cfg->address_bits = 8;
    slave_hd_cfg->dummy_bits = 8;
    slave_hd_cfg->queue_size = 4;
    slave_hd_cfg->dma_chan = SPI_DMA_CH_AUTO;

    // master writes to shared buffer
    slave_hd_cfg->cb_config.cb_buffer_rx = cb_master_write_buffer;
    slave_hd_cfg->cb_config.cb_recv = NULL;
}

static void init_slave_hd(void)
{
    spi_bus_config_t bus_cfg = {};
    spi_bus_default_config(&bus_cfg);

    spi_slave_hd_slot_config_t slave_hd_cfg = {};
    spi_slot_default_config(&slave_hd_cfg);

    ESP_ERROR_CHECK(spi_slave_hd_init(SLAVE_HOST, &bus_cfg, &slave_hd_cfg));
}

static esp_err_t esp32s2_spi_init(void)
{
    pxMutex = xSemaphoreCreateMutex();
#ifdef CONFIG_SPI_STREAM_MODE
    spi_slave_rx_ring_buf = xStreamBufferCreate(SPI_READ_STREAM_BUFFER, 1024);
    spi_slave_tx_ring_buf = xStreamBufferCreate(SPI_WRITE_STREAM_BUFFER, 1024);
#elif defined(CONFIG_SPI_PACKET_MODE)
    spi_slave_rx_ring_buf = xRingbufferCreate(SPI_READ_STREAM_BUFFER, RINGBUF_TYPE_NOSPLIT);
    spi_slave_tx_ring_buf = xRingbufferCreate(SPI_WRITE_STREAM_BUFFER, RINGBUF_TYPE_NOSPLIT);
#endif

    if (spi_slave_rx_ring_buf == NULL || spi_slave_tx_ring_buf == NULL) {
        // There was not enough heap memory space available to create
        ESP_LOGE(TAG, "creat StreamBuffer error, free heap heap: %d", esp_get_free_heap_size());
        assert(0);
    }

    ESP_LOGI(TAG, "init gpio");
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = SPI_SLAVE_HANDSHARK_SEL;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 0);

    // init slave driver
    init_slave_hd();

    msg_queue = xQueueCreate(10, sizeof(spi_msg_t));
    if (!msg_queue) {
        ESP_LOGE(TAG, "Semaphore create error");
        return ESP_FAIL;
    }

    xTaskCreate(spi_transmit_task, "spi_transmit_task", 2048, NULL, 8, NULL);

    return ESP_OK;
}

static int spi_open(const char* path, int flags, int mode)
{
    // init slave driver
    esp32s2_spi_init();

    return ESP_OK;
}

static int spi_close(int fd)
{
    spi_slave_hd_deinit(SLAVE_HOST);
    return 0;
}

static ssize_t spi_read(int fd, void* data, size_t len)
{
    uint32_t ring_len = 0;
#ifdef CONFIG_SPI_STREAM_MODE
    ring_len = xStreamBufferReceive(spi_slave_rx_ring_buf, (void*) data, len, 0);
#elif defined(CONFIG_SPI_PACKET_MODE)
    uint8_t* transmit_point = NULL;
    spi_mutex_lock();
    transmit_point = xRingbufferReceive(spi_slave_rx_ring_buf, &ring_len, 0);
    memcpy(data, transmit_point, ring_len);
    vRingbufferReturnItem(spi_slave_rx_ring_buf, transmit_point);
    spi_mutex_unlock();
#endif
    if (ring_len != len) {
        ESP_LOGD(TAG, "Read len expect %d, but actual read %d", len, ring_len);
    }
    return ring_len;
}

static ssize_t spi_write(int fd, const void* data, size_t size)
{
    uint32_t length = 0;
#ifdef CONFIG_SPI_STREAM_MODE
    if (data == NULL  || size > SPI_WRITE_STREAM_BUFFER || size == 0) {
#elif defined(CONFIG_SPI_PACKET_MODE)
    // Each item stored in no-split/allow-split buffers will require an additional 8 bytes for a header.
    if (data == NULL  || size > (SPI_WRITE_STREAM_BUFFER - 8) || size == 0) {
#endif
        ESP_LOGE(TAG, "Write data error, len:%d", size);
        return -1;
    }
#ifdef CONFIG_SPI_STREAM_MODE
    length = xStreamBufferSend(spi_slave_tx_ring_buf, data, size, portMAX_DELAY);
#elif defined(CONFIG_SPI_PACKET_MODE)
    if (xRingbufferSend(spi_slave_tx_ring_buf, (void*)data, size, portMAX_DELAY) == pdFALSE) {
        ESP_LOGE(TAG, "Send len %d to buffer error, please enlarge the TX buffer size", size);
        return -1;
    }
#endif

    spi_mutex_lock();

#ifdef CONFIG_SPI_STREAM_MODE
    length = xStreamBufferBytesAvailable(spi_slave_tx_ring_buf);
#elif defined(CONFIG_SPI_PACKET_MODE)
    vRingbufferGetInfo(spi_slave_tx_ring_buf, NULL, NULL, NULL, NULL, &length);
#endif

    if (initiative_send_flag == 0 && length > 0) {
        initiative_send_flag = 1;
        spi_msg_t spi_msg = {
            .direct = SPI_SLAVE_WR,
        };

        if (xQueueSend(msg_queue, (void*)&spi_msg, 0) != pdPASS) {
            ESP_LOGE(TAG, "send WR queue for spi_write error");
        }
    }
    spi_mutex_unlock();

    return size;
}

bool esp_vfs_instance_want_write(void)
{
    bool ret;
#ifdef CONFIG_SPI_STREAM_MODE
    if (xStreamBufferBytesAvailable(spi_slave_rx_ring_buf) > 0) {
#elif defined(CONFIG_SPI_PACKET_MODE)
    vRingbufferGetInfo(spi_slave_rx_ring_buf, NULL, NULL, NULL, NULL, &ringbuffer_rx_item_size);
    if (ringbuffer_rx_item_size > 0) {
#endif
        ret = true;
    } else {
        ret = false;
    }

    return ret;
}

void vfs_register_transmit_instance(esp_vfs_t *vfs)
{
    if (vfs == NULL) {
        ESP_LOGE(TAG, "vfs handle uninited");
        return;
    }
    vfs->write = &spi_write;
    vfs->open = &spi_open;
    vfs->close = &spi_close;
    vfs->read = &spi_read;
}
#endif