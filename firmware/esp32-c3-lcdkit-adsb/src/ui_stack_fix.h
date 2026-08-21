#pragma once

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"

/* Green CRT theme layered over the proven ADS-B/PPI implementation. */
static constexpr uint32_t ADSB_CRT_BG = 0x001507;
static constexpr uint32_t ADSB_CRT_GREEN = 0x00ff55;
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
    /* build_ui() has completed before adsb-ui is created; allow LVGL to settle. */
    vTaskDelay(pdMS_TO_TICKS(900));
    if (bsp_display_lock(500)) {
        lv_obj_t *screen = lv_screen_active();
        if (screen) {
            lv_obj_set_style_bg_color(screen, lv_color_hex(ADSB_CRT_BG), 0);
            lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

            /* The first four full-size children are Overview/PPI/Nearest/Receiver. */
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
    /* Styles remain attached to LVGL objects; release this task's RAM. */
    vTaskDelete(nullptr);
}

/*
 * Keep MQTT/parser sizes unchanged. Only adsb-ui gets extra stack for the
 * richer PPI target/vector rendering. The one-shot theme task frees itself.
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
        xTaskCreate(adsb_crt_theme_task, "adsb-theme", 2048, nullptr, 2, nullptr);
    }
    return rc;
}

#define xTaskCreate(...) adsb_xTaskCreate(__VA_ARGS__)
