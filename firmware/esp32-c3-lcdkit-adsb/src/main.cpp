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
#include "cJSON.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "config.h"

static const char *TAG = "adsb-display";
static constexpr uint32_t RADAR_GREEN = 0x00ff66;
static constexpr uint32_t RADAR_GREEN_MED = 0x00a844;
static constexpr uint32_t RADAR_GREEN_DIM = 0x174d2a;

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
static lv_group_t *g_nav_group = nullptr;
static lv_obj_t *g_nav_buttons[4]{};
static lv_obj_t *g_radar_scope = nullptr;
static lv_obj_t *g_sweep_dots[10]{};
static int g_page = 0;
static const int g_radar_ranges[] = {25, 50, 100, 200, 400};
static int g_radar_range_idx = 4;
static int g_sweep_deg = 0;
static int64_t g_last_sweep_ms = 0;
static char g_alert[40]{};
static int64_t g_alert_until_ms = 0;

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u", where,
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
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }
    if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100))) {
        g_plane_count = 0;
        cJSON *item = nullptr;
        cJSON_ArrayForEach(item, root) {
            if (g_plane_count >= 24) break;
            if (cJSON_IsObject(item)) parse_plane(item, g_planes[g_plane_count++]);
        }
        xSemaphoreGive(g_data_mutex);
        ESP_LOGI(TAG, "aircraft update: %u planes", (unsigned)g_plane_count);
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
                         ? flight->valuestring
                         : (cJSON_IsString(sq) ? sq->valuestring : "");
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
            char topic[96];
            snprintf(topic, sizeof(topic), "%s/summary", MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(g_mqtt, topic, 0);
            snprintf(topic, sizeof(topic), "%s/aircraft", MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(g_mqtt, topic, 0);
            snprintf(topic, sizeof(topic), "%s/alert", MQTT_BASE_TOPIC);
            esp_mqtt_client_subscribe(g_mqtt, topic, 0);
            g_ui_dirty = true;
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            g_mqtt_connected = false;
            g_ui_dirty = true;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA:
            if (e && e->current_data_offset == 0 && e->data_len == e->total_data_len) {
                if (topic_ends_with(e, "/summary")) handle_summary(e->data, e->data_len);
                else if (topic_ends_with(e, "/aircraft")) handle_aircraft(e->data, e->data_len);
                else if (topic_ends_with(e, "/alert")) handle_alert(e->data, e->data_len);
            }
            break;
        default:
            break;
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
    if (!g_mqtt) return false;
    if (esp_mqtt_client_register_event(g_mqtt, MQTT_EVENT_ANY, mqtt_event, nullptr) != ESP_OK) return false;
    log_heap("after mqtt init");
    return true;
}

static void start_mqtt()
{
    if (!g_mqtt || g_mqtt_started) return;
    if (esp_mqtt_client_start(g_mqtt) == ESP_OK) {
        g_mqtt_started = true;
        ESP_LOGI(TAG, "MQTT client started");
    }
}

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        g_mqtt_connected = false;
        g_ui_dirty = true;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true;
        g_ui_dirty = true;
        ESP_LOGI(TAG, "Wi-Fi connected");
        start_mqtt();
    }
}

static void wifi_init()
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta() ? ESP_OK : ESP_FAIL);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int y, uint32_t color = 0xffffff)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_align(obj, LV_ALIGN_TOP_MID, 0, y);
    return obj;
}

static void draw_title(lv_obj_t *screen, const char *text)
{
    make_label(screen, text, 10, g_page == 1 ? RADAR_GREEN : 0x00d7ff);
}

static void draw_overview(lv_obj_t *screen, const Summary &s)
{
    draw_title(screen, "ADS-B");
    lv_obj_t *main = make_label(screen, "", 48);
    lv_label_set_text_fmt(main, "%d AIRCRAFT", s.aircraft);
    lv_obj_set_style_text_color(main, s.source_online ? lv_color_hex(0x50e050) : lv_color_hex(0xff4040), 0);
    lv_obj_t *info = make_label(screen, "", 82);
    lv_label_set_text_fmt(info, "POS %d\nMSG/S %.0f\nMAX %.0f km", s.with_position, s.msg_rate, s.max_range_km);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    const AircraftDot &a = s.nearest;
    const char *id = a.flight[0] ? a.flight : (a.hex[0] ? a.hex : "---");
    lv_obj_t *near = make_label(screen, "", 150);
    lv_label_set_text_fmt(near, "NEAREST %s\n%.1f km %d ft", id, a.distance_km, a.altitude_ft);
    lv_obj_set_style_text_align(near, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_nearest(lv_obj_t *screen, const Summary &s)
{
    draw_title(screen, "NEAREST");
    const AircraftDot &a = s.nearest;
    const char *id = a.flight[0] ? a.flight : (a.hex[0] ? a.hex : "---");
    lv_obj_t *o = make_label(screen, "", 52);
    lv_label_set_text_fmt(o, "%s\n%.1f km\n%d ft\n%.0f kt\nTRK %d\nSQ %s",
                          id, a.distance_km, a.altitude_ft, a.speed_kt, a.track,
                          a.squawk[0] ? a.squawk : "----");
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_status(lv_obj_t *screen, const Summary &s)
{
    draw_title(screen, "RECEIVER");
    lv_obj_t *o = make_label(screen, "", 58);
    lv_label_set_text_fmt(o, "%s\n\nWi-Fi %s\nMQTT %s\nData age %d s",
                          s.source_online ? "SOURCE ONLINE" : "SOURCE OFFLINE",
                          g_wifi_connected ? "OK" : "DOWN",
                          g_mqtt_connected ? "OK" : "DOWN", s.age_seconds);
    lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    return o;
}

static void radar_ring(lv_obj_t *parent, int diameter)
{
    lv_obj_t *ring = plain_obj(parent);
    lv_obj_set_size(ring, diameter, diameter);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(RADAR_GREEN_DIM), 0);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
}

static void radar_target(lv_obj_t *parent, int x, int y, bool emergency)
{
    lv_obj_t *dot = plain_obj(parent);
    lv_obj_set_size(dot, emergency ? 7 : 5, emergency ? 7 : 5);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, emergency ? lv_color_hex(0xff3030) : lv_color_hex(RADAR_GREEN), 0);
    lv_obj_align(dot, LV_ALIGN_CENTER, x, y);
}

static void create_sweep(lv_obj_t *scope)
{
    for (int i = 0; i < 10; ++i) {
        g_sweep_dots[i] = plain_obj(scope);
        lv_obj_set_size(g_sweep_dots[i], 3, 3);
        lv_obj_set_style_radius(g_sweep_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(g_sweep_dots[i], LV_OPA_COVER, 0);
        uint32_t c = i > 6 ? RADAR_GREEN : (i > 2 ? RADAR_GREEN_MED : RADAR_GREEN_DIM);
        lv_obj_set_style_bg_color(g_sweep_dots[i], lv_color_hex(c), 0);
    }
}

static void update_sweep_locked()
{
    if (g_page != 1 || !g_radar_scope) return;
    float a = (g_sweep_deg - 90.0f) * 3.14159265f / 180.0f;
    for (int i = 0; i < 10; ++i) {
        float rr = 7.0f + i * 7.0f;
        int x = (int)(cosf(a) * rr);
        int y = (int)(sinf(a) * rr);
        if (g_sweep_dots[i]) lv_obj_align(g_sweep_dots[i], LV_ALIGN_CENTER, x, y);
    }
    g_sweep_deg = (g_sweep_deg + 6) % 360;
}

static void draw_radar(lv_obj_t *screen, const AircraftDot *planes, size_t count)
{
    const float range = (float)g_radar_ranges[g_radar_range_idx];
    char title[32];
    snprintf(title, sizeof(title), "RADAR %d km", (int)range);
    draw_title(screen, title);

    g_radar_scope = plain_obj(screen);
    lv_obj_set_size(g_radar_scope, 174, 174);
    lv_obj_align(g_radar_scope, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_radius(g_radar_scope, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_radar_scope, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_radar_scope, lv_color_hex(0x001108), 0);
    lv_obj_set_style_border_width(g_radar_scope, 2, 0);
    lv_obj_set_style_border_color(g_radar_scope, lv_color_hex(RADAR_GREEN_MED), 0);
    lv_obj_clear_flag(g_radar_scope, LV_OBJ_FLAG_SCROLLABLE);

    radar_ring(g_radar_scope, 58);
    radar_ring(g_radar_scope, 116);
    radar_ring(g_radar_scope, 170);

    lv_obj_t *north = lv_label_create(g_radar_scope);
    lv_label_set_text(north, "N");
    lv_obj_set_style_text_color(north, lv_color_hex(RADAR_GREEN), 0);
    lv_obj_align(north, LV_ALIGN_TOP_MID, 0, 3);

    radar_target(g_radar_scope, 0, 0, false);

    size_t shown = 0;
    for (size_t i = 0; i < count; ++i) {
        if (planes[i].distance_km <= 0 || planes[i].distance_km > range) continue;
        float angle = (planes[i].bearing - 90.0f) * 3.14159265f / 180.0f;
        float rr = (planes[i].distance_km / range) * 80.0f;
        int x = (int)(cosf(angle) * rr);
        int y = (int)(sinf(angle) * rr);
        bool emergency = !strcmp(planes[i].squawk, "7500") ||
                         !strcmp(planes[i].squawk, "7600") ||
                         !strcmp(planes[i].squawk, "7700");
        radar_target(g_radar_scope, x, y, emergency);
        shown++;
    }

    create_sweep(g_radar_scope);
    update_sweep_locked();

    lv_obj_t *count_label = make_label(screen, "", 211, RADAR_GREEN_MED);
    lv_label_set_text_fmt(count_label, "%u/%u targets", (unsigned)shown, (unsigned)count);
}

static void nav_event(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        if (g_page != page) {
            g_page = page;
            g_ui_dirty = true;
        }
    } else if (code == LV_EVENT_CLICKED) {
        if (page == 1) {
            g_radar_range_idx = (g_radar_range_idx + 1) % 5;
            g_ui_dirty = true;
        }
    }
}

static void setup_encoder_navigation(lv_obj_t *screen)
{
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_ENCODER) return;

    if (g_nav_group) {
        lv_group_delete(g_nav_group);
        g_nav_group = nullptr;
    }
    g_nav_group = lv_group_create();

    for (int i = 0; i < 4; ++i) {
        g_nav_buttons[i] = lv_button_create(screen);
        lv_obj_remove_style_all(g_nav_buttons[i]);
        lv_obj_set_size(g_nav_buttons[i], 1, 1);
        lv_obj_set_pos(g_nav_buttons[i], 0, 0);
        lv_obj_set_style_bg_opa(g_nav_buttons[i], LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(g_nav_buttons[i], nav_event, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(g_nav_buttons[i], nav_event, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_group_add_obj(g_nav_group, g_nav_buttons[i]);
    }
    lv_indev_set_group(indev, g_nav_group);
    lv_group_focus_obj(g_nav_buttons[g_page]);
}

static void render_ui()
{
    Summary summary;
    AircraftDot planes[24];
    size_t count = 0;
    if (!xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100))) return;
    summary = g_summary;
    count = g_plane_count;
    memcpy(planes, g_planes, sizeof(planes));
    xSemaphoreGive(g_data_mutex);

    if (!bsp_display_lock(1000)) return;
    lv_obj_t *screen = lv_screen_active();
    if (g_nav_group) {
        lv_indev_t *indev = bsp_display_get_input_dev();
        if (indev) lv_indev_set_group(indev, nullptr);
        lv_group_delete(g_nav_group);
        g_nav_group = nullptr;
    }
    lv_obj_clean(screen);
    memset(g_sweep_dots, 0, sizeof(g_sweep_dots));
    g_radar_scope = nullptr;
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (g_page == 0) draw_overview(screen, summary);
    else if (g_page == 1) draw_radar(screen, planes, count);
    else if (g_page == 2) draw_nearest(screen, summary);
    else draw_status(screen, summary);

    if (g_alert[0] && now_ms() < g_alert_until_ms) {
        lv_obj_t *alert = make_label(screen, g_alert, 200, 0xffffff);
        lv_obj_set_style_bg_color(alert, lv_color_hex(0xc00000), 0);
        lv_obj_set_style_bg_opa(alert, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(alert, 4, 0);
    }

    setup_encoder_navigation(screen);
    bsp_display_unlock();
}

static void ui_task(void *)
{
    while (true) {
        if (g_alert[0] && now_ms() >= g_alert_until_ms) {
            g_alert[0] = 0;
            g_ui_dirty = true;
        }
        if (g_ui_dirty) {
            g_ui_dirty = false;
            render_ui();
        }
        if (g_page == 1 && now_ms() - g_last_sweep_ms >= 80) {
            g_last_sweep_ms = now_ms();
            if (bsp_display_lock(50)) {
                update_sweep_locked();
                bsp_display_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "ADS-B LCDkit ESP-IDF/BSP build starting");
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    g_data_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(g_data_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    if (!prepare_mqtt()) ESP_LOGE(TAG, "MQTT preparation failed");

    ESP_LOGI(TAG, "Starting official ESP32-C3-LCDkit BSP display");
    g_display = bsp_display_start();
    ESP_ERROR_CHECK(g_display ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_LOGI(TAG, "Using BSP encoder input device");

    xTaskCreate(ui_task, "adsb-ui", 4096, nullptr, 4, nullptr);

    ESP_LOGI(TAG, "Starting Wi-Fi");
    wifi_init();
}
