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
#pragma once

#include <stdbool.h>
#include "esp_vfs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* 0 ~ 2 id used for UART*/
    VFS_DEV_SPI_LOCAL_FD = 3,               /*!< SPI dev fd*/
    VFS_DEV_SDIO_LOCAL_FD = 4,              /*!< SDIO dev fd*/
    VFS_DEV_LOCAL_FD_MAX,                   /*!< vfs dev max index*/
} vfs_dev_local_fd;

typedef void (*dev_select_notif_callback_t)(int fd);

/**
 * @brief The VFS instance has data to write
 *
 * This function will unblock select function
 *
 * @return  true if the registration is successful,
 *          ESP_ERR_INVALID_ARG if the arguments are incorrect.
 */
bool esp_vfs_instance_want_write(void);

/**
 * @brief Set notification callback function for select() events
 * @param fd dev fd
 * @param dev_select_notif_callback callback function
 */
void dev_set_select_notif_callback(int fd, dev_select_notif_callback_t dev_select_notif_callback);

/**
 * Register a virtual filesystem, just register open/read/write/close function.
 *
 * @param vfs  Pointer to esp_vfs_t, a structure which maps syscalls to
 *             the filesystem driver functions.
 */
void vfs_register_transmit_instance(esp_vfs_t *vfs);

#ifdef __cplusplus
}
#endif