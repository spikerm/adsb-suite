#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
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
static esp_mqtt_client_handle_t g_mqtt = nullptr;
static lv_display_t *g_display = nullptr;
static int g_page = 0;
static const int g_radar_ranges[] = {25, 50, 100, 200, 400};
static int g_radar_range_idx = 2;
static char g_alert[40]{};
static int64_t g_alert_until_ms = 0;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

static void json_copy(char *dst, size_t dst_len, const cJSON *item)
{
    if (!dst || dst_len == 0) return;
    dst[0] = 0;
    if (cJSON_IsString(item) && item->valuestring) {
        strlcpy(dst, item->valuestring, dst_len);
    }
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

static bool topic_ends_with(esp_mqtt_event_handle_t event, const char *suffix)
{
    const size_t sl = strlen(suffix);
    if ((size_t)event->topic_len < sl) return false;
    return memcmp(event->topic + event->topic_len - sl, suffix, sl) == 0;
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
        const cJSON *online = cJSON_GetObjectItemCaseSensitive(root, "source_online");
        g_summary.source_online = cJSON_IsTrue(online);
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
    auto event = (esp_mqtt_event_handle_t)event_data;
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
            snprintf(topic, sizeof(topic), "%s/status", MQTT_BASE_TOPIC);
            esp_mqtt_client_publish(g_mqtt, topic, "online", 0, 0, 1);
            g_ui_dirty = true;
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT disconnected");
            g_mqtt_connected = false;
            g_ui_dirty = true;
            break;
        case MQTT_EVENT_DATA:
            if (topic_ends_with(event, "/summary")) handle_summary(event->data, event->data_len);
            else if (topic_ends_with(event, "/aircraft")) handle_aircraft(event->data, event->data_len);
            else if (topic_ends_with(event, "/alert")) handle_alert(event->data, event->data_len);
            break;
        default:
            break;
    }
}

static void start_mqtt()
{
    if (g_mqtt) return;
    static char uri[128];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", MQTT_HOST, MQTT_PORT);

    esp_mqtt_client_config_t cfg = {};
    cfg.broker.address.uri = uri;
    cfg.credentials.client_id = DEVICE_NAME;
    if (strlen(MQTT_USERNAME)) {
        cfg.credentials.username = MQTT_USERNAME;
        cfg.credentials.authentication.password = MQTT_PASSWORD;
    }
    cfg.buffer.size = 8192;

    g_mqtt = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(g_mqtt, MQTT_EVENT_ANY, mqtt_event, nullptr));
    ESP_ERROR_CHECK(esp_mqtt_client_start(g_mqtt));
}

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false;
        g_mqtt_connected = false;
        g_ui_dirty = true;
        vTaskDelay(pdMS_TO_TICKS(500));
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
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));

    wifi_config_t wifi_cfg = {};
    strlcpy((char *)wifi_cfg.sta.ssid, WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

static void draw_title(lv_obj_t *screen, const char *title)
{
    lv_obj_t *label = make_label(screen, title, 12);
    lv_obj_set_style_text_color(label, lv_color_hex(0x00d7ff), 0);
}

static void draw_overview(lv_obj_t *screen, const Summary &s)
{
    draw_title(screen, "ADS-B");
    lv_obj_t *main = make_label(screen, "", 48);
    lv_label_set_text_fmt(main, "%d AIRCRAFT", s.aircraft);
    lv_obj_set_style_text_color(main, s.source_online ? lv_color_hex(0x50e050) : lv_color_hex(0xff4040), 0);

    lv_obj_t *info = make_label(screen, "", 82);
    lv_label_set_text_fmt(info, "POS  %d\nMSG/S  %.0f\nMAX  %.0f km", s.with_position, s.msg_rate, s.max_range_km);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);

    const AircraftDot &a = s.nearest;
    const char *id = a.flight[0] ? a.flight : (a.hex[0] ? a.hex : "---");
    lv_obj_t *near = make_label(screen, "", 150);
    lv_label_set_text_fmt(near, "NEAREST  %s\n%.1f km  %d ft", id, a.distance_km, a.altitude_ft);
    lv_obj_set_style_text_align(near, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_nearest(lv_obj_t *screen, const Summary &s)
{
    draw_title(screen, "NEAREST");
    const AircraftDot &a = s.nearest;
    const char *id = a.flight[0] ? a.flight : (a.hex[0] ? a.hex : "---");
    lv_obj_t *label = make_label(screen, "", 52);
    lv_label_set_text_fmt(label, "%s\n%.1f km\n%d ft\n%.0f kt\nTRK %d\nSQ %s",
                          id, a.distance_km, a.altitude_ft, a.speed_kt, a.track,
                          a.squawk[0] ? a.squawk : "----");
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_status(lv_obj_t *screen, const Summary &s)
{
    draw_title(screen, "RECEIVER");
    lv_obj_t *label = make_label(screen, "", 58);
    lv_label_set_text_fmt(label, "%s\n\nWi-Fi  %s\nMQTT  %s\nData age  %d s",
                          s.source_online ? "SOURCE ONLINE" : "SOURCE OFFLINE",
                          g_wifi_connected ? "OK" : "DOWN",
                          g_mqtt_connected ? "OK" : "DOWN",
                          s.age_seconds);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void draw_radar(lv_obj_t *screen, const AircraftDot *planes, size_t count)
{
    char title[32];
    const float range = (float)g_radar_ranges[g_radar_range_idx];
    snprintf(title, sizeof(title), "RADAR  %d km", (int)range);
    draw_title(screen, title);

    const int d = 164;
    lv_obj_t *radar = lv_obj_create(screen);
    lv_obj_set_size(radar, d, d);
    lv_obj_align(radar, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_radius(radar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(radar, lv_color_black(), 0);
    lv_obj_set_style_border_color(radar, lv_color_hex(0x606060), 0);
    lv_obj_set_style_border_width(radar, 1, 0);
    lv_obj_set_style_pad_all(radar, 0, 0);
    lv_obj_clear_flag(radar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *n = lv_label_create(radar);
    lv_label_set_text(n, "N");
    lv_obj_set_style_text_color(n, lv_color_hex(0x00d7ff), 0);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *center = lv_obj_create(radar);
    lv_obj_set_size(center, 6, 6);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(0x00d7ff), 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, 0);

    const float radius = 74.0f;
    for (size_t i = 0; i < count; ++i) {
        if (planes[i].distance_km <= 0 || planes[i].distance_km > range) continue;
        float angle = (planes[i].bearing - 90.0f) * (float)M_PI / 180.0f;
        float rr = (planes[i].distance_km / range) * radius;
        int x = (int)(cosf(angle) * rr);
        int y = (int)(sinf(angle) * rr);
        bool emergency = strcmp(planes[i].squawk, "7500") == 0 ||
                         strcmp(planes[i].squawk, "7600") == 0 ||
                         strcmp(planes[i].squawk, "7700") == 0;
        lv_obj_t *dot = lv_obj_create(radar);
        lv_obj_set_size(dot, emergency ? 7 : 5, emergency ? 7 : 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, emergency ? lv_color_hex(0xff3030) : lv_color_white(), 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_CENTER, x, y);
    }
}

static void render_ui()
{
    Summary summary;
    AircraftDot planes[24];
    size_t plane_count = 0;

    if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100))) {
        summary = g_summary;
        plane_count = g_plane_count;
        memcpy(planes, g_planes, sizeof(planes));
        xSemaphoreGive(g_data_mutex);
    } else {
        return;
    }

    if (!bsp_display_lock(1000)) return;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (g_page == 0) draw_overview(screen, summary);
    else if (g_page == 1) draw_radar(screen, planes, plane_count);
    else if (g_page == 2) draw_nearest(screen, summary);
    else draw_status(screen, summary);

    if (g_alert[0] && now_ms() < g_alert_until_ms) {
        lv_obj_t *alert = lv_label_create(screen);
        lv_label_set_text(alert, g_alert);
        lv_obj_set_style_text_color(alert, lv_color_white(), 0);
        lv_obj_set_style_bg_color(alert, lv_color_hex(0xc00000), 0);
        lv_obj_set_style_bg_opa(alert, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(alert, 5, 0);
        lv_obj_align(alert, LV_ALIGN_BOTTOM_MID, 0, -8);
    }

    bsp_display_unlock();
}

static void encoder_task(void *)
{
    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << BSP_ENCODER_A) | (1ULL << BSP_ENCODER_B) | (1ULL << BSP_ENCODER_PRESS);
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&io));

    int last_a = gpio_get_level(BSP_ENCODER_A);
    int last_button = 1;
    int64_t last_button_ms = 0;

    while (true) {
        int a = gpio_get_level(BSP_ENCODER_A);
        if (a != last_a && a == 0) {
            int dir = gpio_get_level(BSP_ENCODER_B) ? 1 : -1;
            if (g_page == 1) {
                g_radar_range_idx += dir;
                if (g_radar_range_idx < 0) g_radar_range_idx = 4;
                if (g_radar_range_idx > 4) g_radar_range_idx = 0;
            } else {
                g_page += dir;
                if (g_page < 0) g_page = 3;
                if (g_page > 3) g_page = 0;
            }
            g_ui_dirty = true;
        }
        last_a = a;

        int button = gpio_get_level(BSP_ENCODER_PRESS);
        if (last_button == 1 && button == 0 && now_ms() - last_button_ms > 300) {
            last_button_ms = now_ms();
            g_page = (g_page + 1) % 4;
            g_ui_dirty = true;
        }
        last_button = button;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
        vTaskDelay(pdMS_TO_TICKS(100));
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

    ESP_LOGI(TAG, "Starting official ESP32-C3-LCDkit BSP display");
    g_display = bsp_display_start();
    ESP_ERROR_CHECK(g_display ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    if (bsp_display_lock(1000)) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_t *label = lv_label_create(screen);
        lv_label_set_text(label, "ADS-B\nstarting...");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        bsp_display_unlock();
    }

    xTaskCreate(encoder_task, "encoder", 3072, nullptr, 5, nullptr);
    xTaskCreate(ui_task, "adsb-ui", 4096, nullptr, 4, nullptr);

    ESP_LOGI(TAG, "Starting Wi-Fi");
    wifi_init();
}
