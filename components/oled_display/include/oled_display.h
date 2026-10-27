#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#define OLED_DEFAULT_SDA  17
#define OLED_DEFAULT_SCL  18
#elif defined(CONFIG_IDF_TARGET_ESP32)
#define OLED_DEFAULT_SDA  21
#define OLED_DEFAULT_SCL  22
#else
#define OLED_DEFAULT_SDA  5
#define OLED_DEFAULT_SCL  6
#endif
#define OLED_I2C_ADDR     0x3C

/**
 * @brief Initialize display from NVS config.
 * Starts the display update task.
 */
void oled_display_init(void);

/**
 * @brief Enable display (persisted to NVS, requires reboot).
 */
void oled_display_enable(void);

/**
 * @brief Disable display (persisted to NVS, requires reboot).
 */
void oled_display_disable(void);

/**
 * @brief Set I2C GPIO pins (persisted to NVS, requires reboot).
 */
void oled_display_set_gpio(int sda, int scl);

/**
 * @brief Get current display config.
 * @param[out] enabled  Whether display is enabled
 * @param[out] sda      SDA GPIO pin
 * @param[out] scl      SCL GPIO pin
 */
void oled_display_get_config(bool *enabled, int *sda, int *scl);

#else /* !CONFIG_IDF_TARGET_ESP32 && !CONFIG_IDF_TARGET_ESP32C3 && !CONFIG_IDF_TARGET_ESP32S3 */

static inline void oled_display_init(void) {}
static inline void oled_display_enable(void) {}
static inline void oled_display_disable(void) {}
static inline void oled_display_set_gpio(int sda, int scl) { (void)sda; (void)scl; }
static inline void oled_display_get_config(bool *enabled, int *sda, int *scl) {
    if (enabled) *enabled = false;
    if (sda) *sda = 0;
    if (scl) *scl = 0;
}

#endif

#ifdef __cplusplus
}
#endif
