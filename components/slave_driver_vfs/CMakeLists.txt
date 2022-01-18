if (NOT "${IDF_TARGET}" STREQUAL "esp32s3")

set(srcs  "src/vfs_spi_io.c" 
          "src/vfs_sdio_io.c")

if (CONFIG_IDF_TARGET_ESP32)
    list(APPEND srcs "src/port/esp32/sdio_slave_io.c")
elseif(CONFIG_IDF_TARGET_ESP8266)
    list(APPEND srcs "src/port/esp8266/spi_slave_io.c")
else()
    list(APPEND srcs "src/port/esp32_series/spi_slave_io.c")
endif()

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS "include"
                    PRIV_INCLUDE_DIRS "include_private")
endif()