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

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "esp8266/spi_struct.h"
#include "esp8266/gpio_struct.h"
#include "esp_system.h"
#include "esp_log.h"

#include "esp_vfs.h"

#include "driver/gpio.h"
#include "driver/spi.h"

static const char* TAG = "spi_slave";

#define SPI_TASK_STACK           2048
#define SPI_TASK_PRIORITY        8

#define SPI_SLAVE_HANDSHARK_GPIO     CONFIG_SPI_HANDSHAKE_PIN
#define SPI_SLAVE_HANDSHARK_SEL      (1ULL<<SPI_SLAVE_HANDSHARK_GPIO)

#define SPI_WRITE_STREAM_BUFFER      CONFIG_TX_STREAM_BUFFER_SIZE
#define SPI_READ_STREAM_BUFFER       CONFIG_RX_STREAM_BUFFER_SIZE

#define SPI_WRITE_MAX        CONFIG_TX_STREAM_BUFFER_SIZE    // one time max send count

//#define LOG_LOCAL_LEVEL 5     //debug mode, it will print debug log
//#define  SPI_DEBUG_MODE

typedef enum {
    SPI_SLAVE_NULL = 0,
    SPI_SLAVE_SEND,         // slave -> master
    SPI_SLAVE_RECV,             // maste -> slave
} spi_slave_trans_mode_t;

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

static uint16_t spi_slave_send_len = 0;

static uint16_t master_send_len = 0;
static bool master_send_flag = false;

static spi_slave_trans_mode_t current_trans_mode = SPI_SLAVE_NULL;

static StreamBufferHandle_t spi_slave_tx_ring_buf;
static StreamBufferHandle_t spi_slave_rx_ring_buf;
static bool wait_recv_data = false;
static bool sending_flag = false;

static bool spi_init_flag = false;

static uint32_t last_intr_val = 0;

// defined in vfs_xx_io.c
extern esp_vfs_select_sem_t* _signal_sem;

inline static uint8_t count_one_bits(uint32_t value)
{
    uint8_t ones;

    for (ones = 0; value != 0; value >>= 1) {
        if ((value & 1) != 0) {
            ones += 1;
        }
    }

    return ones;
}

inline static void IRAM_ATTR write_rd_status_data(spi_slave_trans_mode_t spi_mode, uint16_t transmit_len)
{
    ESP_EARLY_LOGV(TAG, "write rd status: %d, %d", (uint32_t)spi_mode, transmit_len);
    spi_rd_status_opt_t rd_status_opt;
    rd_status_opt.direct = spi_mode;
    current_trans_mode = spi_mode;
    if (spi_mode == SPI_SLAVE_SEND) {    // slave -> master
        rd_status_opt.seq_num = ++spi_slave_send_seq_num;
        spi_slave_send_len = transmit_len;
    } else {                             // SPI_SLAVE_RECV   maste -> slave
        rd_status_opt.seq_num = ++spi_slave_recv_seq_num;
        master_send_len = transmit_len;
    }
    rd_status_opt.transmit_len = transmit_len;
    spi_slave_set_status(HSPI_HOST, (uint32_t*)&rd_status_opt);
}

inline static void IRAM_ATTR load_send_buffer(uint8_t* data, uint32_t len)
{
    spi_trans_t trans = {0};
    uint16_t cmd = 0;
    memset(&trans, 0x0, sizeof(trans));
    trans.cmd = &cmd;
    trans.addr = NULL;
    trans.bits.val = 0;
    trans.bits.cmd = 8 * 1;
    trans.bits.addr = 8 * 1;
    trans.bits.mosi = 0;
    trans.miso = data;
    trans.bits.miso = len << 3;
    spi_trans(HSPI_HOST, &trans);
}

static void IRAM_ATTR spi_event_callback(int event, void* arg)
{
    int x;
    BaseType_t xHigherPriorityTaskWoken;
    //at_spi_send_opt_t send_opt;
    spi_wr_status_opt_t wr_status_opt;
    uint32_t trans_done;
    uint32_t data[16];
    bool trigger_flag = false;
    uint32_t send_len = 0;

    switch (event) {
        case SPI_TRANS_DONE_EVENT: {
            trans_done = *(uint32_t*)arg;
            ESP_EARLY_LOGV(TAG, "Intr: %x", trans_done);
            //if (count_one_bits(trans_done) > 1) {
                //ESP_EARLY_LOGE(TAG, "Err intr: %x", trans_done);
            //}
            gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 0);

            if (trans_done & SPI_SLV_RD_STA_DONE) {   // slave -> master status, BIT(2)
                if (!sending_flag) {
                    ESP_EARLY_LOGE(TAG, "Receive unexpected RD status");
                    break;
                }

                if (last_intr_val == trans_done) {
                    ESP_EARLY_LOGE(TAG, "Repeat RD_STA interrput");
                }

                if (current_trans_mode == SPI_SLAVE_SEND) {
                    send_len = spi_slave_send_len > 64 ? 64 : spi_slave_send_len;
                    send_len = xStreamBufferReceiveFromISR(spi_slave_tx_ring_buf, data, send_len, &xHigherPriorityTaskWoken);
                    if (send_len == 0) {
                        ESP_EARLY_LOGE(TAG, "Slave sent but len is 0");
                        break;
                    }
                    load_send_buffer((uint8_t*)data, send_len);
                    spi_slave_send_len -= send_len;
                }

                trigger_flag = true;
            }          

            if (trans_done & SPI_SLV_RD_BUF_DONE) {   // slave -> master data  , BIT(0)
                if (spi_slave_send_len > 0) {
                    send_len = spi_slave_send_len > 64 ? 64 : spi_slave_send_len;
                    send_len = xStreamBufferReceiveFromISR(spi_slave_tx_ring_buf, data, send_len, &xHigherPriorityTaskWoken);
                    if (send_len == 0) {
                        ESP_EARLY_LOGE(TAG, "Slave sent but len is 0");
                        break;
                    }
                    load_send_buffer((uint8_t*)data, send_len);
                    spi_slave_send_len -= send_len;
                    trigger_flag = true;
                } else {
                    // slave send done, not triger intr again
                    ESP_EARLY_LOGV(TAG, "slave send done");

                    spi_slave_get_status(HSPI_HOST, (uint32_t*)&wr_status_opt);
                    if (wr_status_opt.send_seq == ((spi_slave_recv_seq_num + 1) & 0xFF)) {
                        ESP_EARLY_LOGD(TAG, "WR BUF slave recv seq:%x, len:%x", wr_status_opt.send_seq, wr_status_opt.send_len);
                        
                        if (wr_status_opt.send_len > SPI_READ_STREAM_BUFFER || wr_status_opt.send_len == 0) {
                            ESP_EARLY_LOGE(TAG, "err recv len:%d", wr_status_opt.send_len);
                            break;
                        }
                        master_send_flag = true;
                        write_rd_status_data(SPI_SLAVE_RECV, wr_status_opt.send_len);
                        trigger_flag = true;
                        if (trans_done & SPI_SLV_WR_STA_DONE) {
                            ESP_EARLY_LOGE(TAG, "RD_BUF, not allow write status");
                            break;
                        }
                    } else {
                        spi_slave_send_len = xStreamBufferBytesAvailable(spi_slave_tx_ring_buf);
                        if (spi_slave_send_len > 0) {      // Stream buffer have data, send again
                            write_rd_status_data(SPI_SLAVE_SEND, spi_slave_send_len);
                            trigger_flag = true;
                        } else {                // no read or write data
                            sending_flag = false;    
                        }   
                    }                      

                }
            }

            if (trans_done & SPI_SLV_WR_BUF_DONE) {   // master -> slave data  , BIT(1)
                if (master_send_len == 0) {
                    ESP_EARLY_LOGE(TAG, "WR_BUF, master send len is 0");
                    break;
                }

                for (x = 0; x < 16; x++) {
                    data[x] = SPI1.data_buf[x];
                }

                uint32_t len = master_send_len > 64 ? 64 : master_send_len;

                xStreamBufferSendFromISR(spi_slave_rx_ring_buf, (void*) data, len, &xHigherPriorityTaskWoken);
                master_send_len -= len;  

                if (master_send_len == 0) {
                    master_send_flag = false;
                    
                    if (_signal_sem != NULL) {
                        esp_vfs_select_triggered_isr(*_signal_sem, &xHigherPriorityTaskWoken);
                    }

                    // check slave data
                    spi_slave_send_len = xStreamBufferBytesAvailable(spi_slave_tx_ring_buf);
                    if (spi_slave_send_len > 0) {      // Stream buffer have data, send again
                        ESP_EARLY_LOGD(TAG, "WR_BUF, slave send");
                        write_rd_status_data(SPI_SLAVE_SEND, spi_slave_send_len);
                        trigger_flag = true; 
                    } else {   // master has already write WR_STATUS 
                        sending_flag = false;    
                    } 

                } else {
                    if (xStreamBufferSpacesAvailable(spi_slave_rx_ring_buf) >= 128) {      // Stream buffer not full, can be read agian
                        trigger_flag = true;
                    } else {
                        ESP_EARLY_LOGD(TAG, "Rx buffer full");
                        wait_recv_data = true;
                    }
                }
            }

            if (trans_done & SPI_SLV_WR_STA_DONE) {        // master -> slave status len  BIT(3)
                spi_slave_get_status(HSPI_HOST, (uint32_t*)&wr_status_opt);
                if (master_send_flag || wr_status_opt.send_seq == (spi_slave_recv_seq_num & 0xFF)) {
                    ESP_EARLY_LOGE(TAG, "Repeat WR_STA intrrupt");
                    trigger_flag = true;
                } else {
                    //ESP_EARLY_LOGE(TAG, "WR status, %x, %d", wr_status_opt.send_len, count_one_bits(trans_done));
                    if (wr_status_opt.magic != 0xFE) {
                        ESP_EARLY_LOGE(TAG, "err magic:%d", wr_status_opt.magic);
                        break;
                    }

                    if (wr_status_opt.send_seq != ((spi_slave_recv_seq_num + 1) & 0xFF)) {
                        ESP_EARLY_LOGE(TAG, "err slave recv seq:%x,%x", wr_status_opt.send_seq, (spi_slave_recv_seq_num +(uint8_t)1));
                        break;
                    }

                    if (wr_status_opt.send_len > SPI_READ_STREAM_BUFFER || wr_status_opt.send_len == 0) {
                        ESP_EARLY_LOGE(TAG, "err recv len:%d", wr_status_opt.send_len);
                        break;
                    }
                    master_send_flag = true;

                    if (!sending_flag) {
                        write_rd_status_data(SPI_SLAVE_RECV, wr_status_opt.send_len);
                        sending_flag = true;
                        trigger_flag = true;
                    } else {
                        master_send_len = wr_status_opt.send_len;
                    }

                }
            }
            last_intr_val = trans_done;

            if (trigger_flag) {
                gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 1);
            }

            if (xHigherPriorityTaskWoken == pdTRUE) {
                taskYIELD();
            }
        }
        break;

    }

}            

static void esp8266_spi_slave_init(void)
{
    spi_slave_tx_ring_buf = xStreamBufferCreate(SPI_WRITE_STREAM_BUFFER, 1);
    spi_slave_rx_ring_buf = xStreamBufferCreate(SPI_READ_STREAM_BUFFER, 1024);

    ESP_LOGI(TAG, "init gpio");
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = SPI_SLAVE_HANDSHARK_SEL;
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 0);

    ESP_LOGI(TAG, "init spi, this is SPI new driver");

    spi_config_t spi_config;
    // Load default interface parameters
    // CS_EN:1, MISO_EN:1, MOSI_EN:1, BYTE_TX_ORDER:1, BYTE_TX_ORDER:1, BIT_RX_ORDER:0, BIT_TX_ORDER:0, CPHA:0, CPOL:0
    spi_config.interface.val = SPI_DEFAULT_INTERFACE | CONFIG_SPI_MODE;

#ifdef CONFIG_MOSI_BYTE_LITTLE_ENDIAN
    spi_config.interface.byte_tx_order = 1;
#endif

#ifdef CONFIG_MISO_BYTE_LITTLE_ENDIAN
    spi_config.interface.byte_rx_order = 1;
#endif

#ifdef CONFIG_MOSI_BIT_ORDER_LSB
    spi_config.interface.bit_tx_order = 1;
#endif

#ifdef CONFIG_MISO_BIT_ORDER_LSB
    spi_config.interface.bit_rx_order = 1;
#endif
    // Load default interrupt enable
    // TRANS_DONE: false, WRITE_STATUS: true, READ_STATUS: true, WRITE_BUFFER: true, READ_BUFFER: ture
    //spi_config.intr_enable.val = SPI_SLAVE_DEFAULT_INTR_ENABLE;
    spi_config.intr_enable.val = 0xF;   // diable READ_STATUS
    // Set SPI to slave mode
    spi_config.mode = SPI_SLAVE_MODE;

    // Register SPI event callback function
    spi_config.event_cb = spi_event_callback;

    spi_init(HSPI_HOST, &spi_config);

#ifdef CONFIG_SPI_DUAL_MODE
    SPI1.ctrl.fread_dual = 1;
    SPI1.user.fwrite_dual = 1;
#endif

    ESP_LOGI(TAG, "slave ready");

}

static int spi_open(const char* path, int flags, int mode)
{
    // this is fairly primitive, we should check if file is opened read only,
    // and error out if write is requested
    int fd = -1;
    char path_postfix[3];

    sprintf(path_postfix, "/%d", CONFIG_SPI_NUM);

    if (strcmp(path, path_postfix) == 0) {
        fd = 0;
    } else {
        return -1;
    }

    if (!spi_init_flag) {
        spi_init_flag = true;
    } else {
        return -1;
    }

    // init slave driver
    esp8266_spi_slave_init();

    return fd;
}

static int spi_close(int fd)
{
    if (spi_init_flag) {
        spi_init_flag = false;
    } else {
        return ESP_FAIL;
    }
    spi_deinit(HSPI_HOST);
    vStreamBufferDelete(spi_slave_tx_ring_buf);
    vStreamBufferDelete(spi_slave_rx_ring_buf);

    return 0;
}

static ssize_t spi_read(int fd, void* data, size_t len)
{
    uint32_t copy_len = 0;

    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "Cannot get read data address.");
        return -1;
    }

    copy_len = xStreamBufferReceive(spi_slave_rx_ring_buf, data, len, 0);
    if (copy_len > 0) {
        // steam buffer full
        if (wait_recv_data) {
            if (xStreamBufferSpacesAvailable(spi_slave_rx_ring_buf) > (SPI_READ_STREAM_BUFFER >> 1)) {
                gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 1);
                wait_recv_data = false;
            }
        }
    }
    return copy_len;
}

static ssize_t spi_write(int fd, const void* data, size_t size)
{
    int32_t length = 0;

    if (data == NULL  || size >= SPI_WRITE_STREAM_BUFFER) {
        ESP_LOGE(TAG, "Write data error, len:%d", size);
        return -1;
    }

    length += xStreamBufferSend(spi_slave_tx_ring_buf, data, size, portMAX_DELAY);
    portENTER_CRITICAL();
    uint32_t remain_len = xStreamBufferBytesAvailable(spi_slave_tx_ring_buf);
    if (sending_flag == false && remain_len > 0) {
        ESP_LOGD(TAG, "SPI write len:%d", remain_len);
        gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 0);
        write_rd_status_data(SPI_SLAVE_SEND, remain_len);
        gpio_set_level(SPI_SLAVE_HANDSHARK_GPIO, 1);
        sending_flag = true;
    }

    portEXIT_CRITICAL();
    return length;
}

bool esp_vfs_instance_want_write(void)
{
    if (xStreamBufferBytesAvailable(spi_slave_rx_ring_buf) > 0) {
        return true;
    } else {
        return false;
    }
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
