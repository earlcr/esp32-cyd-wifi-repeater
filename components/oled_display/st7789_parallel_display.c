/* ST7789 8-bit Parallel Display Driver for ESP32-2432S022 Smart Display
 * Integrated into ESP32 NAT Router
 */

#include "oled_display.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/ip4_addr.h"
#include "font5x7.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "st7789_disp";

/* Pin definitions for ESP32-2432S022 */
#define PIN_LCD_BL     0
#define PIN_LCD_RD     2
#define PIN_LCD_WR     4
#define PIN_LCD_DC     16
#define PIN_LCD_CS     17
#define PIN_LCD_D0     15
#define PIN_LCD_D1     13
#define PIN_LCD_D2     12
#define PIN_LCD_D3     14
#define PIN_LCD_D4     27
#define PIN_LCD_D5     25
#define PIN_LCD_D6     33
#define PIN_LCD_D7     32

#define LCD_WIDTH      240
#define LCD_HEIGHT     320
#define BAND_HEIGHT    40
#define NUM_BANDS      (LCD_HEIGHT / BAND_HEIGHT) // 8 bands

/* RGB565 Colors */
#define COLOR_BLACK       0x0000
#define COLOR_WHITE       0xFFFF
#define COLOR_DARK_BLUE   0x0014
#define COLOR_NAVY        0x08F0
#define COLOR_CARD_BG     0x10A2
#define COLOR_CARD_BORDER 0x2965
#define COLOR_CYAN        0x07FF
#define COLOR_GREEN       0x07E0
#define COLOR_AMBER       0xFDC0
#define COLOR_RED         0xF800
#define COLOR_GREY        0x8410
#define COLOR_LIGHT_GREY  0xC618

/* Extern router globals */
extern bool ap_connect;
extern uint16_t connect_count;
extern uint32_t my_ip;
extern uint32_t my_ap_ip;
extern char *ssid;
extern char *ap_ssid;
extern char *ap_passwd;
extern uint64_t sta_bytes_sent;
extern uint64_t sta_bytes_received;
extern void resync_connect_count(void);
extern bool vpn_is_connected(void);

static esp_lcd_panel_handle_t panel_handle = NULL;
static uint16_t *band_buf = NULL;

/* Low-level drawing primitives into band buffer */
static inline void set_pixel(int x, int y, int y_start, uint16_t color) {
    if (x >= 0 && x < LCD_WIDTH && y >= y_start && y < (y_start + BAND_HEIGHT)) {
        band_buf[(y - y_start) * LCD_WIDTH + x] = color;
    }
}

static void fill_rect(int rx, int ry, int rw, int rh, int y_start, uint16_t color) {
    int x1 = rx < 0 ? 0 : rx;
    int x2 = (rx + rw) > LCD_WIDTH ? LCD_WIDTH : (rx + rw);
    int y1 = ry < y_start ? y_start : ry;
    int y2 = (ry + rh) > (y_start + BAND_HEIGHT) ? (y_start + BAND_HEIGHT) : (ry + rh);

    for (int y = y1; y < y2; y++) {
        int row_idx = (y - y_start) * LCD_WIDTH;
        for (int x = x1; x < x2; x++) {
            band_buf[row_idx + x] = color;
        }
    }
}

static void draw_rect(int rx, int ry, int rw, int rh, int y_start, uint16_t color) {
    fill_rect(rx, ry, rw, 1, y_start, color);
    fill_rect(rx, ry + rh - 1, rw, 1, y_start, color);
    fill_rect(rx, ry, 1, rh, y_start, color);
    fill_rect(rx + rw - 1, ry, 1, rh, y_start, color);
}

static void draw_char(int x, int y, char c, uint16_t color, int scale, int y_start) {
    if (c < 0x20 || c > 0x7E) c = '?';
    int font_idx = (c - 0x20) * 5;

    for (int col = 0; col < 5; col++) {
        uint8_t line = font5x7[font_idx + col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                fill_rect(x + col * scale, y + row * scale, scale, scale, y_start, color);
            }
        }
    }
}

static void draw_string(int x, int y, const char *str, uint16_t color, int scale, int y_start) {
    if (!str) return;
    int cur_x = x;
    int char_w = 6 * scale;
    while (*str) {
        if (cur_x + char_w > LCD_WIDTH) break;
        draw_char(cur_x, y, *str, color, scale, y_start);
        cur_x += char_w;
        str++;
    }
}

/* Render one full frame (all 8 bands) */
static void render_dashboard(void) {
    if (!panel_handle || !band_buf) return;

    char ip_str[32] = "0.0.0.0";
    if (my_ip != 0) {
        ip4_addr_t addr;
        addr.addr = my_ip;
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&addr));
    }

    char ap_ip_str[32] = "192.168.4.1";
    if (my_ap_ip != 0) {
        ip4_addr_t ap_addr;
        ap_addr.addr = my_ap_ip;
        snprintf(ap_ip_str, sizeof(ap_ip_str), IPSTR, IP2STR(&ap_addr));
    }

    char client_str[64];
    snprintf(client_str, sizeof(client_str), "Clients: %u connected", connect_count);

    char traffic_str[64];
    double mb_rx = (double)sta_bytes_received / (1024.0 * 1024.0);
    double mb_tx = (double)sta_bytes_sent / (1024.0 * 1024.0);
    snprintf(traffic_str, sizeof(traffic_str), "RX:%.1fMB TX:%.1fMB", mb_rx, mb_tx);

    const char *uplink_ssid = (ssid && ssid[0]) ? ssid : "Scanning...";
    const char *hotspot_ssid = (ap_ssid && ap_ssid[0]) ? ap_ssid : "ESP32_NAT_Router";
    const char *hotspot_pass = (ap_passwd && ap_passwd[0]) ? ap_passwd : "open / none";
    bool is_connected = (my_ip != 0);

    for (int b = 0; b < NUM_BANDS; b++) {
        int y_start = b * BAND_HEIGHT;

        // Clear band with background (Dark navy / black)
        for (int i = 0; i < LCD_WIDTH * BAND_HEIGHT; i++) {
            band_buf[i] = 0x0842; // Deep dark navy #0B111E
        }

        /* 1. Header Banner (y = 0..38) */
        fill_rect(0, 0, LCD_WIDTH, 36, y_start, COLOR_NAVY);
        fill_rect(0, 36, LCD_WIDTH, 2, y_start, COLOR_CYAN);
        draw_string(24, 7, "ESP32 WI-FI REPEATER", COLOR_WHITE, 2, y_start);
        draw_string(68, 24, "NAT BRIDGE & ROUTER", COLOR_CYAN, 1, y_start);

        /* 2. Card 1: Upstream Home Wi-Fi (y = 44..124) */
        fill_rect(6, 44, LCD_WIDTH - 12, 80, y_start, COLOR_CARD_BG);
        draw_rect(6, 44, LCD_WIDTH - 12, 80, y_start, COLOR_CARD_BORDER);
        draw_string(14, 50, "[ UPLINK WI-FI ]", COLOR_CYAN, 1, y_start);
        draw_string(14, 64, uplink_ssid, COLOR_WHITE, 2, y_start);
        if (is_connected) {
            draw_string(14, 86, "Status: CONNECTED", COLOR_GREEN, 1, y_start);
            char ip_disp[64];
            snprintf(ip_disp, sizeof(ip_disp), "IP: %s", ip_str);
            draw_string(14, 102, ip_disp, COLOR_LIGHT_GREY, 1, y_start);
        } else {
            draw_string(14, 86, "Status: CONNECTING...", COLOR_AMBER, 1, y_start);
            draw_string(14, 102, "IP: Waiting for DHCP", COLOR_GREY, 1, y_start);
        }

        /* 3. Card 2: Repeater Hotspot (y = 130..220) */
        fill_rect(6, 130, LCD_WIDTH - 12, 90, y_start, COLOR_CARD_BG);
        draw_rect(6, 130, LCD_WIDTH - 12, 90, y_start, COLOR_CARD_BORDER);
        draw_string(14, 136, "[ EXTENDED REPEATER AP ]", COLOR_GREEN, 1, y_start);
        draw_string(14, 150, hotspot_ssid, COLOR_WHITE, 2, y_start);
        char pass_disp[64];
        snprintf(pass_disp, sizeof(pass_disp), "Password: %s", hotspot_pass);
        draw_string(14, 172, pass_disp, COLOR_AMBER, 1, y_start);
        char ap_ip_disp[64];
        snprintf(ap_ip_disp, sizeof(ap_ip_disp), "Gateway:  %s", ap_ip_str);
        draw_string(14, 188, ap_ip_disp, COLOR_LIGHT_GREY, 1, y_start);
        draw_string(14, 204, "Security: WPA2-Personal", COLOR_GREY, 1, y_start);

        /* 4. Card 3: Live Stats (y = 226..290) */
        fill_rect(6, 226, LCD_WIDTH - 12, 64, y_start, COLOR_CARD_BG);
        draw_rect(6, 226, LCD_WIDTH - 12, 64, y_start, COLOR_CARD_BORDER);
        draw_string(14, 232, "[ ACTIVE TRAFFIC & CLIENTS ]", COLOR_CYAN, 1, y_start);
        draw_string(14, 248, client_str, is_connected ? COLOR_GREEN : COLOR_WHITE, 1, y_start);
        draw_string(14, 262, traffic_str, COLOR_LIGHT_GREY, 1, y_start);
        draw_string(14, 276, "Routing:  NAPT Enabled", COLOR_CYAN, 1, y_start);

        /* 5. Footer (y = 296..318) */
        draw_string(24, 302, "Admin: http://192.168.4.1", COLOR_CYAN, 1, y_start);

        // Send band to ST7789 display
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y_start, LCD_WIDTH, y_start + BAND_HEIGHT, band_buf);
    }
}

static void display_task(void *pvParameters) {
    ESP_LOGI(TAG, "Display refresh task started");
    while (1) {
        render_dashboard();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void oled_display_init(void) {
    ESP_LOGI(TAG, "Initializing ST7789 parallel display for ESP32-2432S022...");

    // 1. Initialize Backlight pin (GPIO 0) and Read Strobe (GPIO 2)
    gpio_config_t bl_conf = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL) | (1ULL << PIN_LCD_RD),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_conf);
    gpio_set_level(PIN_LCD_BL, 1); // Backlight ON
    gpio_set_level(PIN_LCD_RD, 1); // RD HIGH (inactive)

    // 2. Configure 8-bit Intel 8080 parallel bus
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .data_gpio_nums = {
            PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3,
            PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7
        },
        .bus_width = 8,
        .max_transfer_bytes = LCD_WIDTH * BAND_HEIGHT * sizeof(uint16_t),
        .dma_burst_size = 64,
    };
    esp_err_t err = esp_lcd_new_i80_bus(&bus_config, &i80_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create i80 bus: %s", esp_err_to_name(err));
        return;
    }

    // 3. Configure Panel IO
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 12 * 1000 * 1000, // 12 MHz
        .trans_queue_depth = 10,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
        .flags = {
            .swap_color_bytes = 0,
        },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel io: %s", esp_err_to_name(err));
        return;
    }

    // 4. Configure ST7789 panel
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1, // Reset tied to EN
        .rgb_endian = LCD_RGB_ENDIAN_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create st7789 panel: %s", esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    // 5. Allocate band buffer (19,200 bytes)
    band_buf = (uint16_t *)heap_caps_malloc(LCD_WIDTH * BAND_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!band_buf) {
        band_buf = (uint16_t *)malloc(LCD_WIDTH * BAND_HEIGHT * sizeof(uint16_t));
    }
    if (!band_buf) {
        ESP_LOGE(TAG, "Failed to allocate display band buffer");
        return;
    }

    // 6. Draw initial frame immediately
    render_dashboard();

    // 7. Start FreeRTOS update task
    xTaskCreatePinnedToCore(display_task, "display_task", 3072, NULL, 3, NULL, 1);
    ESP_LOGI(TAG, "ST7789 display initialized successfully!");
}

void oled_display_enable(void) {}
void oled_display_disable(void) {}
void oled_display_set_gpio(int sda, int scl) { (void)sda; (void)scl; }
void oled_display_get_config(bool *enabled, int *sda, int *scl) {
    if (enabled) *enabled = true;
    if (sda) *sda = 0;
    if (scl) *scl = 0;
}
