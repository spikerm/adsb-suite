#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "mqtt_client.h"
#include "driver/gpio.h"
#include "cJSON.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "config.h"

static const char *TAG = "adsb-display";

struct AircraftDot {
    char flight[10]{};
    char hex[8]{};
    char squawk[5]{};
    float distance_km = 0;
    float bearing = 0;
    float speed_kt = 0;
    int altitude_ft = 0;
    int track = 0;
};

struct Summary {
    int aircraft = 0;
    int with_position = 0;
    float msg_rate = 0;
    float max_range_km = 0;
    bool source_online = false;
    int age_seconds = 0;
    AircraftDot nearest;
};

static Summary g_summary;
static AircraftDot g_planes[24];
static size_t g_plane_count = 0;
static SemaphoreHandle_t g_data_mutex = nullptr;
static volatile bool g_ui_dirty = true;
static volatile bool g_wifi_connected = false;
static volatile bool g_mqtt_connected = false;
static volatile bool g_mqtt_started = false;
static esp_mqtt_client_handle_t g_mqtt = nullptr;
static lv_display_t *g_display = nullptr;
static int g_page = 0;
static const int g_radar_ranges[] = {25, 50, 100, 200, 400};
static int g_radar_range_idx = 2;
static char g_alert[40]{};
static int64_t g_alert_until_ms = 0;

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void json_copy(char *dst, size_t n, const cJSON *item)
{
    if (!dst || !n) return;
    dst[0] = 0;
    if (cJSON_IsString(item) && item->valuestring) strlcpy(dst, item->valuestring, n);
}

static float json_float(const cJSON *o, const char *key, float def = 0)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static int json_int(const cJSON *o, const char *key, int def = 0)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static void parse_plane(const cJSON *o, AircraftDot &a)
{
    memset(&a, 0, sizeof(a));
    json_copy(a.flight, sizeof(a.flight), cJSON_GetObjectItemCaseSensitive(o, "flight"));
    json_copy(a.hex, sizeof(a.hex), cJSON_GetObjectItemCaseSensitive(o, "hex"));
    json_copy(a.squawk, sizeof(a.squawk), cJSON_GetObjectItemCaseSensitive(o, "squawk"));
    a.distance_km = json_float(o, "distance_km");
    a.bearing = json_float(o, "bearing");
    a.speed_kt = json_float(o, "speed_kt");
    a.altitude_ft = json_int(o, "altitude_ft");
    a.track = json_int(o, "track");
}

static bool topic_ends_with(esp_mqtt_event_handle_t e, const char *suffix)
{
    size_t n = strlen(suffix);
    return e && e->topic && e->topic_len >= (int)n &&
           memcmp(e->topic + e->topic_len - n, suffix, n) == 0;
}

static void handle_summary(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100))) {
        g_summary.aircraft = json_int(root, "aircraft");
        g_summary.with_position = json_int(root, "with_position");
        g_summary.msg_rate = json_float(root, "msg_rate");
        g_summary.max_range_km = json_float(root, "max_range_km");
        g_summary.age_seconds = json_int(root, "age_seconds");
        g_summary.source_online = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "source_online"));
        const cJSON *nearest = cJSON_GetObjectItemCaseSensitive(root, "nearest");
        if (cJSON_IsObject(nearest)) parse_plane(nearest, g_summary.nearest);
        xSemaphoreGive(g_data_mutex);
        g_ui_dirty = true;
    }
    cJSON_Delete(root);
}

static void handle_aircraft(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!cJSON_IsArray(root)) { cJSON_Delete(root); return; }
    if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100))) {
        g_plane_count = 0;
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, root) {
            if (g_plane_count >= 24) break;
            if (cJSON_IsObject(item)) parse_plane(item, g_planes[g_plane_count++]);
        }
        xSemaphoreGive(g_data_mutex);
        g_ui_dirty = true;
    }
    cJSON_Delete(root);
}

static void handle_alert(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;
    const cJSON *event = cJSON_GetObjectItemCaseSensitive(root, "event");
    const cJSON *flight = cJSON_GetObjectItemCaseSensitive(root, "flight");
    const cJSON *sq = cJSON_GetObjectItemCaseSensitive(root, "squawk");
    const char *ev = cJSON_IsString(event) ? event->valuestring : "ALERT";
    const char *id = cJSON_IsString(flight) && flight->valuestring && flight->valuestring[0]
                     ? flight->valuestring : (cJSON_IsString(sq) ? sq->valuestring : "");
    snprintf(g_alert, sizeof(g_alert), "%s %s", ev, id ? id : "");
    g_alert_until_ms = now_ms() + 12000;
    g_ui_dirty = true;
    cJSON_Delete(root);
}

static void mqtt_event(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto e = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT connected");
            g_mqtt_connected = true;
            char t[96];
            snprintf(t, sizeof(t), "%s/summary", MQTT_BASE_TOPIC); esp_mqtt_client_subscribe(g_mqtt, t, 0);
            snprintf(t, sizeof(t), "%s/aircraft", MQTT_BASE_TOPIC); esp_mqtt_client_subscribe(g_mqtt, t, 0);
            snprintf(t, sizeof(t), "%s/alert", MQTT_BASE_TOPIC); esp_mqtt_client_subscribe(g_mqtt, t, 0);
            snprintf(t, sizeof(t), "%s/status", MQTT_BASE_TOPIC); esp_mqtt_client_publish(g_mqtt, t, "online", 0, 0, 1);
            g_ui_dirty = true;
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            g_mqtt_connected = false; g_ui_dirty = true; break;
        case MQTT_EVENT_DATA:
            // Publisher payloads fit in our 6 KiB input buffer. Ignore fragmented chunks defensively.
            if (e && e->current_data_offset == 0 && e->data_len == e->total_data_len) {
                if (topic_ends_with(e, "/summary")) handle_summary(e->data, e->data_len);
                else if (topic_ends_with(e, "/aircraft")) handle_aircraft(e->data, e->data_len);
                else if (topic_ends_with(e, "/alert")) handle_alert(e->data, e->data_len);
            } else if (e) {
                ESP_LOGW(TAG, "Ignoring fragmented MQTT message (%d/%d)", e->data_len, e->total_data_len);
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT error");
            break;
        default: break;
    }
}

static bool prepare_mqtt()
{
    if (g_mqtt) return true;
    log_heap("before mqtt init");

    static char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);
    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;
    cfg.credentials.client_id = DEVICE_NAME;
    if (strlen(MQTT_USERNAME)) {
        cfg.credentials.username = MQTT_USERNAME;
        cfg.credentials.authentication.password = MQTT_PASSWORD;
    }
    cfg.buffer.size = 6144;

    g_mqtt = esp_mqtt_client_init(&cfg);
    if (!g_mqtt) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return false;
    }
    esp_err_t err = esp_mqtt_client_register_event(g_mqtt, MQTT_EVENT_ANY, mqtt_event, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MQTT event registration failed: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(g_mqtt);
        g_mqtt = nullptr;
        return false;
    }
    log_heap("after mqtt init");
    ESP_LOGI(TAG, "MQTT client prepared before LVGL/Wi-Fi");
    return true;
}

static void start_mqtt()
{
    if (!g_mqtt || g_mqtt_started) return;
    esp_err_t err = esp_mqtt_client_start(g_mqtt);
    if (err == ESP_OK) {
        g_mqtt_started = true;
        ESP_LOGI(TAG, "MQTT client started");
    } else {
        ESP_LOGE(TAG, "MQTT start failed: %s", esp_err_to_name(err));
    }
}

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false; g_mqtt_connected = false; g_ui_dirty = true;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true; g_ui_dirty = true;
        ESP_LOGI(TAG, "Wi-Fi connected");
        start_mqtt();
    }
}

static void wifi_init()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&c));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    wifi_config_t w = {};
    strlcpy((char *)w.sta.ssid, WIFI_SSID, sizeof(w.sta.ssid));
    strlcpy((char *)w.sta.password, WIFI_PASSWORD, sizeof(w.sta.password));
    w.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    w.sta.pmf_cfg.capable = true;
    w.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static lv_obj_t *label(lv_obj_t *p, const char *text, int y)
{
    lv_obj_t *o = lv_label_create(p);
    lv_label_set_text(o, text);
    lv_obj_set_style_text_color(o, lv_color_white(), 0);
    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, y);
    return o;
}

static void draw_title(lv_obj_t *s, const char *t)
{
    lv_obj_t *o = label(s, t, 12);
    lv_obj_set_style_text_color(o, lv_color_hex(0x00d7ff), 0);
}

static void draw_overview(lv_obj_t *s, const Summary &x)
{
    draw_title(s, "ADS-B");
    lv_obj_t *a = label(s, "", 48);
    lv_label_set_text_fmt(a, "%d AIRCRAFT", x.aircraft);
    lv_obj_set_style_text_color(a, x.source_online ? lv_color_hex(0x50e050) : lv_color_hex(0xff4040), 0);
    lv_obj_t *i = label(s, "", 82);
    lv_label_set_text_fmt(i, "POS  %d\nMSG/S  %.0f\nMAX  %.0f km", x.with_position, x.msg_rate, x.max_range_km);
    lv_obj_set_style_text_align(i, LV_TEXT_ALIGN_CENTER, 0);
    const auto &n = x.nearest;
    const char *id = n.flight[0] ? n.flight : (n.hex[0] ? n.hex : "---");
    lv_obj_t *near = label(s, "", 150);
    lv_label_set_text_fmt(near, "NEAREST  %s\n%.1f km  %d ft", id, n.distance_km, n.altitude_ft);
    lv_obj_set_style_text_align(near, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_nearest(lv_obj_t *s, const Summary &x)
{
    draw_title(s, "NEAREST");
    const auto &a = x.nearest;
    const char *id = a.flight[0] ? a.flight : (a.hex[0] ? a.hex : "---");
    lv_obj_t *o = label(s, "", 52);
    lv_label_set_text_fmt(o, "%s\n%.1f km\n%d ft\n%.0f kt\nTRK %d\nSQ %s",
                          id, a.distance_km, a.altitude_ft, a.speed_kt, a.track, a.squawk[0] ? a.squawk : "----");
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_status(lv_obj_t *s, const Summary &x)
{
    draw_title(s, "RECEIVER");
    lv_obj_t *o = label(s, "", 58);
    lv_label_set_text_fmt(o, "%s\n\nWi-Fi  %s\nMQTT  %s\nData age  %d s",
                          x.source_online ? "SOURCE ONLINE" : "SOURCE OFFLINE",
                          g_wifi_connected ? "OK" : "DOWN",
                          g_mqtt_connected ? "OK" : "DOWN", x.age_seconds);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_radar(lv_obj_t *s, const AircraftDot *p, size_t count)
{
    char title[32];
    float range = (float)g_radar_ranges[g_radar_range_idx];
    snprintf(title, sizeof(title), "RADAR  %d km", (int)range);
    draw_title(s, title);
    lv_obj_t *r = lv_obj_create(s);
    lv_obj_set_size(r, 164, 164); lv_obj_align(r, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x606060), 0);
    lv_obj_set_style_border_width(r, 1, 0); lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *n = lv_label_create(r); lv_label_set_text(n, "N");
    lv_obj_set_style_text_color(n, lv_color_hex(0x00d7ff), 0); lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 2);
    lv_obj_t *c = lv_obj_create(r); lv_obj_set_size(c, 6, 6); lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x00d7ff), 0); lv_obj_set_style_border_width(c, 0, 0); lv_obj_align(c, LV_ALIGN_CENTER, 0, 0);
    for (size_t i = 0; i < count; ++i) {
        if (p[i].distance_km <= 0 || p[i].distance_km > range) continue;
        float a = (p[i].bearing - 90.0f) * 3.14159265f / 180.0f;
        float rr = (p[i].distance_km / range) * 74.0f;
        int x = (int)(cosf(a) * rr), y = (int)(sinf(a) * rr);
        bool em = !strcmp(p[i].squawk, "7500") || !strcmp(p[i].squawk, "7600") || !strcmp(p[i].squawk, "7700");
        lv_obj_t *d = lv_obj_create(r); lv_obj_set_size(d, em ? 7 : 5, em ? 7 : 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, em ? lv_color_hex(0xff3030) : lv_color_white(), 0);
        lv_obj_set_style_border_width(d, 0, 0); lv_obj_align(d, LV_ALIGN_CENTER, x, y);
    }
}

static void render_ui()
{
    Summary s; AircraftDot p[24]; size_t n = 0;
    if (!xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100))) return;
    s = g_summary; n = g_plane_count; memcpy(p, g_planes, sizeof(p)); xSemaphoreGive(g_data_mutex);
    if (!bsp_display_lock(1000)) return;
    lv_obj_t *screen = lv_screen_active(); lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0); lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    if (g_page == 0) draw_overview(screen, s);
    else if (g_page == 1) draw_radar(screen, p, n);
    else if (g_page == 2) draw_nearest(screen, s);
    else draw_status(screen, s);
    if (g_alert[0] && now_ms() < g_alert_until_ms) {
        lv_obj_t *a = lv_label_create(screen); lv_label_set_text(a, g_alert);
        lv_obj_set_style_text_color(a, lv_color_white(), 0); lv_obj_set_style_bg_color(a, lv_color_hex(0xc00000), 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0); lv_obj_set_style_pad_all(a, 5, 0); lv_obj_align(a, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    bsp_display_unlock();
}

static void encoder_task(void *)
{
    int last_a = gpio_get_level(BSP_ENCODER_A), last_button = 1; int64_t last_press = 0;
    while (true) {
        int a = gpio_get_level(BSP_ENCODER_A);
        if (a != last_a && a == 0) {
            int dir = gpio_get_level(BSP_ENCODER_B) ? 1 : -1;
            if (g_page == 1) { g_radar_range_idx += dir; if (g_radar_range_idx < 0) g_radar_range_idx = 4; if (g_radar_range_idx > 4) g_radar_range_idx = 0; }
            else { g_page += dir; if (g_page < 0) g_page = 3; if (g_page > 3) g_page = 0; }
            g_ui_dirty = true;
        }
        last_a = a;
        int b = gpio_get_level(BSP_ENCODER_PRESS);
        if (last_button && !b && now_ms() - last_press > 300) { last_press = now_ms(); g_page = (g_page + 1) % 4; g_ui_dirty = true; }
        last_button = b;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void ui_task(void *)
{
    while (true) {
        if (g_alert[0] && now_ms() >= g_alert_until_ms) { g_alert[0] = 0; g_ui_dirty = true; }
        if (g_ui_dirty) { g_ui_dirty = false; render_ui(); }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ADS-B LCDkit ESP-IDF/BSP build starting");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) { ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init(); }
    ESP_ERROR_CHECK(err);

    g_data_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(g_data_mutex ? ESP_OK : ESP_ERR_NO_MEM);

    // ESP-MQTT creates a private esp_event loop during esp_mqtt_client_init().
    // Allocate it before LVGL and Wi-Fi to avoid heap fragmentation on the C3.
    if (!prepare_mqtt()) ESP_LOGE(TAG, "MQTT preparation failed; UI will still start");

    ESP_LOGI(TAG, "Starting official ESP32-C3-LCDkit BSP display");
    g_display = bsp_display_start();
    ESP_ERROR_CHECK(g_display ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    if (bsp_display_lock(1000)) {
        lv_obj_t *s = lv_screen_active(); lv_obj_set_style_bg_color(s, lv_color_black(), 0); lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_t *l = lv_label_create(s); lv_label_set_text(l, "ADS-B\nstarting...");
        lv_obj_set_style_text_color(l, lv_color_white(), 0); lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        bsp_display_unlock();
    }

    xTaskCreate(encoder_task, "encoder", 3072, nullptr, 5, nullptr);
    xTaskCreate(ui_task, "adsb-ui", 4096, nullptr, 4, nullptr);

    ESP_LOGI(TAG, "Starting Wi-Fi");
    wifi_init();
}
