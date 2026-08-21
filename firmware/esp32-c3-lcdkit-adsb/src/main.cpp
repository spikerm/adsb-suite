#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

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
static constexpr size_t MAX_PLANES = 24;
static constexpr size_t MQTT_RX_MAX = 8192;
static constexpr int RADAR_CX = 120;
static constexpr int RADAR_CY = 120;
static constexpr int RADAR_RADIUS = 112;
static constexpr uint32_t RADAR_GREEN = 0x00ff55;
static constexpr uint32_t RADAR_GREEN_MED = 0x00a83d;
static constexpr uint32_t RADAR_GREEN_DIM = 0x125b2b;
static constexpr uint32_t RADAR_BG = 0x001507;
static constexpr float PI_F = 3.14159265f;

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
static AircraftDot g_planes[MAX_PLANES];
static size_t g_plane_count = 0;
static SemaphoreHandle_t g_data_mutex = nullptr;
static volatile bool g_data_dirty = true;
static volatile bool g_wifi_connected = false;
static volatile bool g_mqtt_connected = false;
static volatile bool g_mqtt_started = false;
static esp_mqtt_client_handle_t g_mqtt = nullptr;
static lv_display_t *g_display = nullptr;

static char g_mqtt_rx[MQTT_RX_MAX]{};
static char g_mqtt_topic[96]{};
static size_t g_mqtt_rx_total = 0;

static int g_page = 0;
static const int g_radar_ranges[] = {25, 50, 100, 200, 400};
static int g_radar_range_idx = 3;
static int g_sweep_deg = 0;
static int64_t g_last_sweep_ms = 0;
static char g_alert[40]{};
static int64_t g_alert_until_ms = 0;

static lv_obj_t *g_pages[4]{};
static lv_group_t *g_nav_group = nullptr;
static lv_obj_t *g_nav_buttons[4]{};
static lv_obj_t *g_overview_main = nullptr;
static lv_obj_t *g_overview_info = nullptr;
static lv_obj_t *g_overview_near = nullptr;
static lv_obj_t *g_nearest_info = nullptr;
static lv_obj_t *g_status_info = nullptr;
static lv_obj_t *g_alert_label = nullptr;

static lv_obj_t *g_radar_title = nullptr;
static lv_obj_t *g_radar_count = nullptr;
static lv_obj_t *g_radar_targets[MAX_PLANES]{};
static bool g_target_active[MAX_PLANES]{};
static float g_target_bearing[MAX_PLANES]{};
static bool g_target_emergency[MAX_PLANES]{};
static lv_obj_t *g_sweep_lines[12]{};
static lv_point_precise_t g_sweep_points[12][2]{};

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

static void log_heap(const char *where)
{
    ESP_LOGI(TAG, "heap %s: free=%u largest=%u", where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void json_copy(char *dst, size_t n, const cJSON *item)
{
    if (!dst || n == 0) return;
    dst[0] = 0;
    if (cJSON_IsString(item) && item->valuestring) strlcpy(dst, item->valuestring, n);
    dst[n - 1] = 0;
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
    if (!isfinite(a.distance_km) || a.distance_km < 0) a.distance_km = 0;
    if (!isfinite(a.bearing)) a.bearing = 0;
    while (a.bearing < 0) a.bearing += 360.0f;
    while (a.bearing >= 360.0f) a.bearing -= 360.0f;
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
        else memset(&g_summary.nearest, 0, sizeof(g_summary.nearest));
        xSemaphoreGive(g_data_mutex);
        g_data_dirty = true;
    }
    cJSON_Delete(root);
}

static void handle_aircraft(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!cJSON_IsArray(root)) {
        ESP_LOGW(TAG, "aircraft JSON parse failed len=%d", len);
        cJSON_Delete(root);
        return;
    }

    AircraftDot temp[MAX_PLANES]{};
    size_t count = 0;
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (count >= MAX_PLANES) break;
        if (cJSON_IsObject(item)) parse_plane(item, temp[count++]);
    }

    if (xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(100))) {
        memcpy(g_planes, temp, sizeof(g_planes));
        g_plane_count = count;
        xSemaphoreGive(g_data_mutex);
        g_data_dirty = true;
    }

    if (count) {
        ESP_LOGI(TAG, "aircraft update: %u planes first=%s %.1fkm %.0fdeg",
                 (unsigned)count,
                 temp[0].flight[0] ? temp[0].flight : temp[0].hex,
                 (double)temp[0].distance_km, (double)temp[0].bearing);
    } else {
        ESP_LOGI(TAG, "aircraft update: 0 planes");
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
    const char *ev = cJSON_IsString(event) && event->valuestring ? event->valuestring : "ALERT";
    const char *id = cJSON_IsString(flight) && flight->valuestring && flight->valuestring[0]
                         ? flight->valuestring
                         : (cJSON_IsString(sq) && sq->valuestring ? sq->valuestring : "");
    snprintf(g_alert, sizeof(g_alert), "%s %s", ev, id);
    g_alert[sizeof(g_alert) - 1] = 0;
    g_alert_until_ms = now_ms() + 12000;
    g_data_dirty = true;
    cJSON_Delete(root);
}

static void dispatch_mqtt_payload(const char *topic, const char *data, int len)
{
    if (strstr(topic, "/summary")) handle_summary(data, len);
    else if (strstr(topic, "/aircraft")) handle_aircraft(data, len);
    else if (strstr(topic, "/alert")) handle_alert(data, len);
}

static void mqtt_event(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    auto e = (esp_mqtt_event_handle_t)event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED: {
            ESP_LOGI(TAG, "MQTT connected");
            g_mqtt_connected = true;
            char topic[96];
            snprintf(topic, sizeof(topic), "%s/summary", MQTT_BASE_TOPIC); esp_mqtt_client_subscribe(g_mqtt, topic, 0);
            snprintf(topic, sizeof(topic), "%s/aircraft", MQTT_BASE_TOPIC); esp_mqtt_client_subscribe(g_mqtt, topic, 0);
            snprintf(topic, sizeof(topic), "%s/alert", MQTT_BASE_TOPIC); esp_mqtt_client_subscribe(g_mqtt, topic, 0);
            g_data_dirty = true;
            break;
        }
        case MQTT_EVENT_DISCONNECTED:
            g_mqtt_connected = false;
            g_data_dirty = true;
            ESP_LOGW(TAG, "MQTT disconnected");
            break;
        case MQTT_EVENT_DATA: {
            if (!e || !e->data || e->data_len <= 0) break;
            if (e->current_data_offset == 0) {
                g_mqtt_rx_total = 0;
                size_t tn = (size_t)e->topic_len < sizeof(g_mqtt_topic) - 1 ? (size_t)e->topic_len : sizeof(g_mqtt_topic) - 1;
                memcpy(g_mqtt_topic, e->topic, tn);
                g_mqtt_topic[tn] = 0;
            }
            size_t off = (size_t)e->current_data_offset;
            size_t n = (size_t)e->data_len;
            if (off + n >= MQTT_RX_MAX) {
                ESP_LOGW(TAG, "MQTT payload too large: %d", e->total_data_len);
                g_mqtt_rx_total = 0;
                break;
            }
            memcpy(g_mqtt_rx + off, e->data, n);
            size_t end = off + n;
            if (end > g_mqtt_rx_total) g_mqtt_rx_total = end;
            if (g_mqtt_rx_total >= (size_t)e->total_data_len) {
                g_mqtt_rx[e->total_data_len] = 0;
                dispatch_mqtt_payload(g_mqtt_topic, g_mqtt_rx, e->total_data_len);
                g_mqtt_rx_total = 0;
            }
            break;
        }
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
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_connected = false; g_mqtt_connected = false; g_data_dirty = true; esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connected = true; g_data_dirty = true; ESP_LOGI(TAG, "Wi-Fi connected"); start_mqtt();
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

static lv_obj_t *plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int y, uint32_t color = 0xffffff)
{
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, text ? text : "");
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_obj_align(o, LV_ALIGN_TOP_MID, 0, y);
    return o;
}

static lv_obj_t *make_page(lv_obj_t *screen)
{
    lv_obj_t *p = plain_obj(screen);
    lv_obj_set_size(p, 240, 240);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, 0);
    return p;
}

static void make_ring(lv_obj_t *parent, int diameter, uint32_t color, int width = 1)
{
    lv_obj_t *r = plain_obj(parent);
    lv_obj_set_size(r, diameter, diameter);
    lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, width, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(color), 0);
    lv_obj_align(r, LV_ALIGN_CENTER, 0, 0);
}

static lv_obj_t *make_line(lv_obj_t *parent, uint32_t color, int width)
{
    lv_obj_t *line = lv_line_create(parent);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    return line;
}

static void page_focus_cb(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    if (page < 0 || page > 3) return;
    g_page = page;
    g_data_dirty = true;
    ESP_LOGI(TAG, "page -> %d", g_page);
}

static void page_click_cb(lv_event_t *e)
{
    int page = (int)(intptr_t)lv_event_get_user_data(e);
    if (page == 1) {
        g_radar_range_idx = (g_radar_range_idx + 1) % 5;
        ESP_LOGI(TAG, "radar range -> %d km", g_radar_ranges[g_radar_range_idx]);
        g_data_dirty = true;
    } else if (g_nav_group) {
        lv_group_focus_next(g_nav_group);
    }
}

static void build_ui()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    g_pages[0] = make_page(screen);
    label(g_pages[0], "ADS-B", 10, 0x00d7ff);
    g_overview_main = label(g_pages[0], "0 AIRCRAFT", 48);
    g_overview_info = label(g_pages[0], "POS 0\nMSG/S 0\nMAX 0 km", 82);
    lv_obj_set_style_text_align(g_overview_info, LV_TEXT_ALIGN_CENTER, 0);
    g_overview_near = label(g_pages[0], "NEAREST ---", 150);
    lv_obj_set_style_text_align(g_overview_near, LV_TEXT_ALIGN_CENTER, 0);

    // Full-screen green PPI radar.
    g_pages[1] = make_page(screen);
    lv_obj_set_style_bg_color(g_pages[1], lv_color_hex(RADAR_BG), 0);
    lv_obj_set_style_bg_opa(g_pages[1], LV_OPA_COVER, 0);
    make_ring(g_pages[1], 236, RADAR_GREEN_MED, 2);
    make_ring(g_pages[1], 180, RADAR_GREEN_DIM);
    make_ring(g_pages[1], 124, RADAR_GREEN_DIM);
    make_ring(g_pages[1], 68, RADAR_GREEN_DIM);

    static lv_point_precise_t hpts[2] = {{8,120},{232,120}};
    static lv_point_precise_t vpts[2] = {{120,8},{120,232}};
    lv_obj_t *h = make_line(g_pages[1], RADAR_GREEN_DIM, 1); lv_line_set_points(h, hpts, 2);
    lv_obj_t *v = make_line(g_pages[1], RADAR_GREEN_DIM, 1); lv_line_set_points(v, vpts, 2);

    for (int i = 0; i < 12; ++i) {
        uint32_t c = i == 0 ? RADAR_GREEN : (i < 4 ? RADAR_GREEN_MED : RADAR_GREEN_DIM);
        g_sweep_lines[i] = make_line(g_pages[1], c, i == 0 ? 2 : 1);
        lv_line_set_points(g_sweep_lines[i], g_sweep_points[i], 2);
        int opa = 255 - i * 19;
        if (opa < 35) opa = 35;
        lv_obj_set_style_line_opa(g_sweep_lines[i], (lv_opa_t)opa, 0);
    }

    for (size_t i = 0; i < MAX_PLANES; ++i) {
        lv_obj_t *d = plain_obj(g_pages[1]);
        lv_obj_set_size(d, 8, 8);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, lv_color_hex(RADAR_GREEN), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_set_style_border_color(d, lv_color_hex(0xb0ffbd), 0);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        g_radar_targets[i] = d;
    }

    g_radar_title = label(g_pages[1], "PPI 200 km", 8, RADAR_GREEN);
    g_radar_count = label(g_pages[1], "0 targets", 216, RADAR_GREEN_MED);

    lv_obj_t *north = lv_label_create(g_pages[1]);
    lv_label_set_text(north, "N");
    lv_obj_set_style_text_color(north, lv_color_hex(RADAR_GREEN), 0);
    lv_obj_set_pos(north, 116, 25);

    g_pages[2] = make_page(screen);
    label(g_pages[2], "NEAREST", 10, 0x00d7ff);
    g_nearest_info = label(g_pages[2], "---", 52);
    lv_obj_set_style_text_align(g_nearest_info, LV_TEXT_ALIGN_CENTER, 0);

    g_pages[3] = make_page(screen);
    label(g_pages[3], "RECEIVER", 10, 0x00d7ff);
    g_status_info = label(g_pages[3], "SOURCE OFFLINE", 58);
    lv_obj_set_style_text_align(g_status_info, LV_TEXT_ALIGN_CENTER, 0);

    g_alert_label = lv_label_create(screen);
    lv_label_set_text(g_alert_label, "");
    lv_obj_set_style_text_color(g_alert_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(g_alert_label, lv_color_hex(0xc00000), 0);
    lv_obj_set_style_bg_opa(g_alert_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_alert_label, 5, 0);
    lv_obj_align(g_alert_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(g_alert_label, LV_OBJ_FLAG_HIDDEN);

    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
        g_nav_group = lv_group_create();
        for (int i = 0; i < 4; ++i) {
            lv_obj_t *btn = lv_button_create(screen);
            lv_obj_set_size(btn, 1, 1);
            lv_obj_set_pos(btn, -10, -10);
            lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(btn, 0, 0);
            lv_obj_set_style_outline_width(btn, 0, 0);
            lv_obj_add_event_cb(btn, page_focus_cb, LV_EVENT_FOCUSED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, page_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_group_add_obj(g_nav_group, btn);
            g_nav_buttons[i] = btn;
        }
        lv_indev_set_group(indev, g_nav_group);
        lv_group_focus_obj(g_nav_buttons[0]);
        ESP_LOGI(TAG, "Using BSP encoder input device");
    }

    for (int i = 1; i < 4; ++i) lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
}

static void set_visible_page(int page)
{
    for (int i = 0; i < 4; ++i) {
        if (i == page) lv_obj_remove_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(g_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static float angle_delta(float a, float b)
{
    float d = fmodf(a - b + 540.0f, 360.0f) - 180.0f;
    return fabsf(d);
}

static void update_sweep()
{
    for (int i = 0; i < 12; ++i) {
        float a = (float)(g_sweep_deg - i * 3 - 90) * PI_F / 180.0f;
        g_sweep_points[i][0].x = RADAR_CX;
        g_sweep_points[i][0].y = RADAR_CY;
        g_sweep_points[i][1].x = RADAR_CX + (int)(cosf(a) * RADAR_RADIUS);
        g_sweep_points[i][1].y = RADAR_CY + (int)(sinf(a) * RADAR_RADIUS);
        lv_line_set_points(g_sweep_lines[i], g_sweep_points[i], 2);
    }

    for (size_t i = 0; i < MAX_PLANES; ++i) {
        if (!g_target_active[i]) continue;
        if (g_target_emergency[i]) {
            lv_obj_set_style_opa(g_radar_targets[i], LV_OPA_COVER, 0);
            continue;
        }
        float d = angle_delta((float)g_sweep_deg, g_target_bearing[i]);
        lv_opa_t opa = d < 6.0f ? LV_OPA_COVER : (d < 30.0f ? 220 : 165);
        lv_obj_set_style_opa(g_radar_targets[i], opa, 0);
    }
}

static void update_ui_data(const Summary &s, const AircraftDot *planes, size_t count)
{
    char buf[192];
    char nid[10];
    char nsq[5];
    strlcpy(nid, s.nearest.flight[0] ? s.nearest.flight : (s.nearest.hex[0] ? s.nearest.hex : "---"), sizeof(nid));
    strlcpy(nsq, s.nearest.squawk[0] ? s.nearest.squawk : "----", sizeof(nsq));

    snprintf(buf, sizeof(buf), "%d AIRCRAFT", s.aircraft);
    lv_label_set_text(g_overview_main, buf);
    lv_obj_set_style_text_color(g_overview_main, s.source_online ? lv_color_hex(0x50e050) : lv_color_hex(0xff4040), 0);

    snprintf(buf, sizeof(buf), "POS %d\nMSG/S %.0f\nMAX %.0f km", s.with_position, (double)s.msg_rate, (double)s.max_range_km);
    lv_label_set_text(g_overview_info, buf);
    snprintf(buf, sizeof(buf), "NEAREST %s\n%.1f km %d ft", nid, (double)s.nearest.distance_km, s.nearest.altitude_ft);
    lv_label_set_text(g_overview_near, buf);

    snprintf(buf, sizeof(buf), "%s\n%.1f km\n%d ft\n%.0f kt\nTRK %d\nSQ %s",
             nid, (double)s.nearest.distance_km, s.nearest.altitude_ft,
             (double)s.nearest.speed_kt, s.nearest.track, nsq);
    lv_label_set_text(g_nearest_info, buf);

    snprintf(buf, sizeof(buf), "%s\n\nWi-Fi %s\nMQTT %s\nData age %d s",
             s.source_online ? "SOURCE ONLINE" : "SOURCE OFFLINE",
             g_wifi_connected ? "OK" : "DOWN",
             g_mqtt_connected ? "OK" : "DOWN", s.age_seconds);
    lv_label_set_text(g_status_info, buf);

    const float range = (float)g_radar_ranges[g_radar_range_idx];
    snprintf(buf, sizeof(buf), "PPI %d km", (int)range);
    lv_label_set_text(g_radar_title, buf);

    size_t shown = 0;
    if (count > MAX_PLANES) count = MAX_PLANES;
    for (size_t i = 0; i < MAX_PLANES; ++i) {
        g_target_active[i] = false;
        g_target_emergency[i] = false;
        if (i >= count || planes[i].distance_km <= 0 || planes[i].distance_km > range) {
            lv_obj_add_flag(g_radar_targets[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        float a = (planes[i].bearing - 90.0f) * PI_F / 180.0f;
        float rr = (planes[i].distance_km / range) * (RADAR_RADIUS - 10);
        int x = RADAR_CX + (int)lroundf(cosf(a) * rr);
        int y = RADAR_CY + (int)lroundf(sinf(a) * rr);
        bool emergency = !strcmp(planes[i].squawk, "7500") ||
                         !strcmp(planes[i].squawk, "7600") ||
                         !strcmp(planes[i].squawk, "7700");
        int sz = emergency ? 10 : 8;
        lv_obj_set_size(g_radar_targets[i], sz, sz);
        lv_obj_set_style_bg_color(g_radar_targets[i], emergency ? lv_color_hex(0xff3030) : lv_color_hex(RADAR_GREEN), 0);
        lv_obj_set_pos(g_radar_targets[i], x - sz / 2, y - sz / 2);
        lv_obj_set_style_opa(g_radar_targets[i], emergency ? LV_OPA_COVER : 180, 0);
        lv_obj_remove_flag(g_radar_targets[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g_radar_targets[i]);
        g_target_active[i] = true;
        g_target_bearing[i] = planes[i].bearing;
        g_target_emergency[i] = emergency;
        ++shown;
    }

    snprintf(buf, sizeof(buf), "%u/%u targets", (unsigned)shown, (unsigned)count);
    lv_label_set_text(g_radar_count, buf);
    lv_obj_move_foreground(g_radar_title);
    lv_obj_move_foreground(g_radar_count);

    if (shown > 0) {
        ESP_LOGI(TAG, "PPI plotted %u/%u targets at range %dkm", (unsigned)shown, (unsigned)count, (int)range);
    }

    if (g_alert[0] && now_ms() < g_alert_until_ms) {
        lv_label_set_text(g_alert_label, g_alert);
        lv_obj_remove_flag(g_alert_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_alert_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_task(void *)
{
    Summary snapshot{};
    AircraftDot planes[MAX_PLANES]{};
    size_t count = 0;
    int last_page = -1;

    while (true) {
        bool need_data = g_data_dirty;
        if (need_data && xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(50))) {
            snapshot = g_summary;
            count = g_plane_count > MAX_PLANES ? MAX_PLANES : g_plane_count;
            memcpy(planes, g_planes, sizeof(planes));
            xSemaphoreGive(g_data_mutex);
            g_data_dirty = false;
        }

        bool sweep_due = (g_page == 1 && now_ms() - g_last_sweep_ms >= 55);
        bool page_changed = g_page != last_page;
        if (need_data || sweep_due || page_changed) {
            if (bsp_display_lock(250)) {
                if (page_changed) { set_visible_page(g_page); last_page = g_page; }
                if (need_data) update_ui_data(snapshot, planes, count);
                if (sweep_due) {
                    g_sweep_deg = (g_sweep_deg + 4) % 360;
                    update_sweep();
                    g_last_sweep_ms = now_ms();
                }
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

    if (bsp_display_lock(1000)) {
        build_ui();
        bsp_display_unlock();
    }

    xTaskCreate(ui_task, "adsb-ui", 6144, nullptr, 4, nullptr);
    ESP_LOGI(TAG, "Starting Wi-Fi");
    wifi_init();
}
