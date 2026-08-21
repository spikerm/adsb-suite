#pragma once

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

/*
 * Runtime CRT/PPI theme helper.
 *
 * The main UI is intentionally left stable: this helper only restyles existing
 * LVGL objects after build_ui() has created them.  That keeps the proven MQTT,
 * parser and PPI plotting paths untouched.
 */
static constexpr uint32_t ADSB_CRT_BG = 0x001507;
static constexpr uint32_t ADSB_CRT_GREEN = 0x00ff55;
static constexpr uint32_t ADSB_CRT_MED = 0x00a83d;
static constexpr uint32_t ADSB_CRT_TEXT = 0xb0ffbd;

static inline bool adsb_text_starts(const char *s, const char *prefix)
{
    return s && prefix && strncmp(s, prefix, strlen(prefix)) == 0;
}

static void adsb_theme_tree(lv_obj_t *obj)
{
    if (!obj) return;

    uint32_t child_count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (!child) continue;

        if (lv_obj_check_type(child, &lv_label_class)) {
            const char *txt = lv_label_get_text(child);
            if (adsb_text_starts(txt, "ADS-B") ||
                adsb_text_starts(txt, "NEAREST") ||
                adsb_text_starts(txt, "RECEIVER") ||
                adsb_text_starts(txt, "PPI")) {
                lv_obj_set_style_text_color(child, lv_color_hex(ADSB_CRT_GREEN), 0);
            } else if (txt && txt[0]) {
                lv_obj_set_style_text_color(child, lv_color_hex(ADSB_CRT_TEXT), 0);
            }
        }
        adsb_theme_tree(child);
    }
}

static void adsb_crt_theme_task(void *)
{
    /* Wait until BSP display, pages and encoder group are fully constructed. */
    vTaskDelay(pdMS_TO_TICKS(1200));

    while (true) {
        if (bsp_display_lock(250)) {
            lv_obj_t *screen = lv_screen_active();
            if (screen) {
                lv_obj_set_style_bg_color(screen, lv_color_hex(ADSB_CRT_BG), 0);
                lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

                /* The first four full-size children are the four application pages. */
                uint32_t n = lv_obj_get_child_count(screen);
                uint32_t pages = n < 4 ? n : 4;
                for (uint32_t i = 0; i < pages; ++i) {
                    lv_obj_t *page = lv_obj_get_child(screen, i);
                    if (!page) continue;
                    lv_obj_set_style_bg_color(page, lv_color_hex(ADSB_CRT_BG), 0);
                    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
                }
                adsb_theme_tree(screen);
            }
            bsp_display_unlock();
        }
        /* New MQTT text inherits the label style, so a slow refresh is enough. */
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/*
 * The richer PPI view increased the call depth of update_ui_data(). Keep
 * MQTT/parser task sizes unchanged and only give the ADS-B UI task headroom.
 * Start the lightweight CRT theme task at the same point, after build_ui().
 */
static inline BaseType_t adsb_xTaskCreate(TaskFunction_t task_code,
                                          const char *name,
                                          configSTACK_DEPTH_TYPE stack_depth,
                                          void *parameters,
                                          UBaseType_t priority,
                                          TaskHandle_t *created_task)
{
    configSTACK_DEPTH_TYPE adjusted_depth = stack_depth;
    bool is_ui = name && strcmp(name, "adsb-ui") == 0;
    if (is_ui && adjusted_depth < 5120) adjusted_depth = 5120;

    BaseType_t rc = xTaskCreate(task_code, name, adjusted_depth,
                                parameters, priority, created_task);
    if (rc == pdPASS && is_ui) {
        /* Small, low-priority task: visual styling only, no ADS-B data copies. */
        xTaskCreate(adsb_crt_theme_task, "adsb-theme", 2048, nullptr, 2, nullptr);
    }
    return rc;
}

#define xTaskCreate(...) adsb_xTaskCreate(__VA_ARGS__)
