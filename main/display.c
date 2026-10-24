#include "display.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define PIN_SCLK 6
#define PIN_MOSI 7
#define PIN_CS   10
#define PIN_DC   2
#define PIN_BL   3
#define PIN_TP_SDA 4
#define PIN_TP_SCL 5
#define PIN_TP_INT 0
#define PIN_TP_RST 1
#define LCD_HOST SPI2_HOST

static esp_lcd_panel_handle_t s_panel;

void display_init(void)
{
    // Backlight on LEDC PWM (a plain GPIO high before this; full brightness until display_set_backlight).
    ledc_timer_config_t bl_timer = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
                              .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&bl_timer);
    ledc_channel_config_t bl_chan = {.gpio_num = PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                .timer_sel = LEDC_TIMER_0, .duty = 255, .hpoint = 0};
    ledc_channel_config(&bl_chan);

    spi_bus_config_t bus = {.sclk_io_num = PIN_SCLK, .mosi_io_num = PIN_MOSI, .miso_io_num = -1,
                            .quadwp_io_num = -1, .quadhd_io_num = -1,
                            .max_transfer_sz = 240 * 20 * 2};
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {.dc_gpio_num = PIN_DC, .cs_gpio_num = PIN_CS,
                                            .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8,
                                            .lcd_param_bits = 8, .spi_mode = 0, .trans_queue_depth = 10};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_dev_config_t pc = {.reset_gpio_num = -1, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
                                     .bits_per_pixel = 16};
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io, &pc, &panel));
    s_panel = panel;
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));  // this panel needs inversion
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lp = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lp));
    const lvgl_port_display_cfg_t dc = {.io_handle = io, .panel_handle = panel,
                                        .buffer_size = 240 * 20, .double_buffer = false,
                                        .hres = 240, .vres = 240, .monochrome = false,
                                        .rotation = {.swap_xy = false, .mirror_x = true, .mirror_y = false},
                                        .flags = {.buff_dma = true}};
    lv_disp_t *disp = lvgl_port_add_disp(&dc);

    // Touch: CST816S on I2C (address 0x15).
    i2c_master_bus_handle_t i2c;
    i2c_master_bus_config_t ic = {.i2c_port = I2C_NUM_0, .sda_io_num = PIN_TP_SDA, .scl_io_num = PIN_TP_SCL,
                                  .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
                                  .flags.enable_internal_pullup = true};
    ESP_ERROR_CHECK(i2c_new_master_bus(&ic, &i2c));
    esp_lcd_panel_io_handle_t tio;
    esp_lcd_panel_io_i2c_config_t tioc = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c, &tioc, &tio));
    esp_lcd_touch_config_t tc = {.x_max = 240, .y_max = 240, .rst_gpio_num = PIN_TP_RST, .int_gpio_num = PIN_TP_INT,
                                 .levels = {.reset = 0, .interrupt = 0},
                                 .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0}};  // chosen on hardware: swipes were reversed with mirror_x = 1
    esp_lcd_touch_handle_t tp;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tio, &tc, &tp));
    const lvgl_port_touch_cfg_t lt = {.disp = disp, .handle = tp};
    lvgl_port_add_touch(&lt);

    // Orientation chosen on hardware (swap=0 mx=1 my=0). Applied last: the lvgl port can reapply its own
    // stored rotation, which is why it also holds mirror_x = true above.
    display_orient(false, true, false);
}

// Applies a panel orientation and forces a full redraw.
void display_orient(bool swap_xy, bool mirror_x, bool mirror_y)
{
    esp_lcd_panel_swap_xy(s_panel, swap_xy);
    esp_lcd_panel_mirror(s_panel, mirror_x, mirror_y);
    lvgl_port_lock(0);
    lv_obj_invalidate(lv_scr_act());
    lvgl_port_unlock();
}

void display_set_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, percent * 255 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
