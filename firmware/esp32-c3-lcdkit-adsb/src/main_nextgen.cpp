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

static const char *TAG = "adsb-nextgen";
static constexpr size_t MAX_PLANES = 24;
static constexpr size_t TRAFFIC_PLANES = 16;
static constexpr size_t MQTT_RX_MAX = 4096;
static constexpr int CX = 120, CY = 120, RR = 112;
static constexpr float PI_F = 3.14159265f;
static constexpr uint32_t C_BG = 0x001006;
static constexpr uint32_t C_GREEN = 0x7CFF38;
static constexpr uint32_t C_GREEN2 = 0x36D83A;
static constexpr uint32_t C_DIM = 0x175C24;
static constexpr uint32_t C_WHITE = 0xD8FFD8;
static constexpr uint32_t C_RED = 0xFF3030;

struct Aircraft {
    char flight[10]{}; char hex[8]{}; char squawk[5]{};
    char origin[5]{}; char destination[5]{};
    float distance_km=0, bearing=0, speed_kt=0;
    int altitude_ft=0, track=0;
};
struct Summary { int aircraft=0, positioned=0, age=0; float msg_rate=0, max_range=0; bool online=false; Aircraft nearest; };

static Summary g_summary;
static Aircraft g_planes[MAX_PLANES], g_parse[MAX_PLANES];
static size_t g_count=0;
static SemaphoreHandle_t g_mutex=nullptr;
static volatile bool g_dirty=true, g_wifi=false, g_mqtt_ok=false, g_mqtt_started=false;
static esp_mqtt_client_handle_t g_mqtt=nullptr;
static TaskHandle_t g_parser=nullptr;
static char g_rx[MQTT_RX_MAX]{}, g_topic[96]{};
static size_t g_rx_have=0; static int g_rx_need=0; static volatile bool g_ready=false, g_drop=false;
static uint32_t g_drops=0; static int64_t g_last_summary=0, g_last_aircraft=0;

static const int g_ranges[]={25,50,100,200,400};
static int g_range_idx=1, g_page=0, g_selected=0, g_sweep=0;
static int64_t g_last_sweep=0;
static bool g_long=false;
static char g_alert[40]{}; static int64_t g_alert_until=0;

static lv_obj_t *pages[5]{}, *nav[5]{}; static lv_group_t *nav_group=nullptr;
static lv_obj_t *ov_count=nullptr,*ov_info=nullptr,*ov_near=nullptr,*near_info=nullptr,*sys_info=nullptr,*alert_label=nullptr;
static lv_obj_t *ppi_title=nullptr,*ppi_count=nullptr,*ppi_sel=nullptr,*ring_label[3]{};
static lv_obj_t *ppi_dot[MAX_PLANES]{}; static bool dot_active[MAX_PLANES]{},dot_emergency[MAX_PLANES]{}; static float dot_bearing[MAX_PLANES]{};
static lv_obj_t *sweep_line[10]{}; static lv_point_precise_t sweep_pts[10][2]{};
static lv_obj_t *sel_vector=nullptr; static lv_point_precise_t sel_vec_pts[2]{};
static lv_obj_t *traffic_title=nullptr,*traffic_sel=nullptr,*traffic_count=nullptr;
static lv_obj_t *traffic_plane[TRAFFIC_PLANES]{}; static lv_point_precise_t traffic_pts[TRAFFIC_PLANES][5]{};

static int64_t now_ms(){return esp_timer_get_time()/1000;}
static void heaplog(const char *s){ESP_LOGI(TAG,"heap %s free=%u largest=%u",s,(unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));}
static void jcopy(char*d,size_t n,const cJSON*i){if(!d||!n)return;d[0]=0;if(cJSON_IsString(i)&&i->valuestring)strlcpy(d,i->valuestring,n);d[n-1]=0;}
static float jf(const cJSON*o,const char*k,float d=0){auto*v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?(float)v->valuedouble:d;}
static int ji(const cJSON*o,const char*k,int d=0){auto*v=cJSON_GetObjectItemCaseSensitive(o,k);return cJSON_IsNumber(v)?v->valueint:d;}
static void parse_aircraft(const cJSON*o,Aircraft&a){
 memset(&a,0,sizeof(a)); jcopy(a.flight,sizeof(a.flight),cJSON_GetObjectItemCaseSensitive(o,"flight")); jcopy(a.hex,sizeof(a.hex),cJSON_GetObjectItemCaseSensitive(o,"hex")); jcopy(a.squawk,sizeof(a.squawk),cJSON_GetObjectItemCaseSensitive(o,"squawk"));
 jcopy(a.origin,sizeof(a.origin),cJSON_GetObjectItemCaseSensitive(o,"origin")); jcopy(a.destination,sizeof(a.destination),cJSON_GetObjectItemCaseSensitive(o,"destination"));
 a.distance_km=jf(o,"distance_km"); a.bearing=jf(o,"bearing",jf(o,"bearing_deg")); a.speed_kt=jf(o,"speed_kt"); a.altitude_ft=ji(o,"altitude_ft"); a.track=ji(o,"track",ji(o,"track_deg"));
 if(!isfinite(a.distance_km)||a.distance_km<0)a.distance_km=0; if(!isfinite(a.bearing))a.bearing=0; while(a.bearing<0)a.bearing+=360; while(a.bearing>=360)a.bearing-=360;
}
static void summary_json(const char*d,int l){cJSON*r=cJSON_ParseWithLength(d,l);if(!cJSON_IsObject(r)){cJSON_Delete(r);return;} Summary s{};s.aircraft=ji(r,"aircraft",ji(r,"count"));s.positioned=ji(r,"with_position",ji(r,"positioned_count"));s.msg_rate=jf(r,"msg_rate");s.max_range=jf(r,"max_range_km");s.age=ji(r,"age_seconds");s.online=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"source_online"))||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r,"source_ok"));auto*n=cJSON_GetObjectItemCaseSensitive(r,"nearest");if(cJSON_IsObject(n))parse_aircraft(n,s.nearest);if(xSemaphoreTake(g_mutex,pdMS_TO_TICKS(100))){g_summary=s;xSemaphoreGive(g_mutex);g_dirty=true;}g_last_summary=now_ms();cJSON_Delete(r);}
static void aircraft_json(const char*d,int l){cJSON*r=cJSON_ParseWithLength(d,l);if(!cJSON_IsArray(r)){ESP_LOGW(TAG,"bad aircraft json len=%d",l);cJSON_Delete(r);return;}memset(g_parse,0,sizeof(g_parse));size_t n=0;cJSON*i=nullptr;cJSON_ArrayForEach(i,r){if(n>=MAX_PLANES)break;if(cJSON_IsObject(i))parse_aircraft(i,g_parse[n++]);}if(xSemaphoreTake(g_mutex,pdMS_TO_TICKS(100))){memcpy(g_planes,g_parse,sizeof(g_planes));g_count=n;xSemaphoreGive(g_mutex);g_dirty=true;}g_last_aircraft=now_ms();ESP_LOGI(TAG,"aircraft parsed=%u",(unsigned)n);cJSON_Delete(r);}
static void alert_json(const char*d,int l){cJSON*r=cJSON_ParseWithLength(d,l);if(!r)return;auto*e=cJSON_GetObjectItemCaseSensitive(r,"event");auto*f=cJSON_GetObjectItemCaseSensitive(r,"flight");snprintf(g_alert,sizeof(g_alert),"%s %s",cJSON_IsString(e)?e->valuestring:"ALERT",cJSON_IsString(f)?f->valuestring:"");g_alert_until=now_ms()+12000;g_dirty=true;cJSON_Delete(r);}
static void dispatch(const char*t,const char*d,int l){if(strstr(t,"/summary"))summary_json(d,l);else if(strstr(t,"/aircraft"))aircraft_json(d,l);else if(strstr(t,"/alert"))alert_json(d,l);}
static void reset_rx(){g_rx_have=0;g_rx_need=0;g_topic[0]=0;g_drop=false;}
static void parser_task(void*){ESP_LOGI(TAG,"parser started");for(;;){ulTaskNotifyTake(pdTRUE,portMAX_DELAY);if(!g_ready)continue;int n=g_rx_need;if(n>0&&n<(int)MQTT_RX_MAX&&g_topic[0])dispatch(g_topic,g_rx,n);reset_rx();g_ready=false;}}
static void mqtt_event(void*,esp_event_base_t,int32_t id,void*data){auto e=(esp_mqtt_event_handle_t)data;switch((esp_mqtt_event_id_t)id){case MQTT_EVENT_CONNECTED:{g_mqtt_ok=true;char t[96];for(const char*s:{"summary","aircraft","alert"}){snprintf(t,sizeof(t),"%s/%s",MQTT_BASE_TOPIC,s);esp_mqtt_client_subscribe(g_mqtt,t,0);}g_dirty=true;ESP_LOGI(TAG,"MQTT connected");break;}case MQTT_EVENT_DISCONNECTED:g_mqtt_ok=false;g_dirty=true;break;case MQTT_EVENT_DATA:{if(!e||!e->data||e->total_data_len<=0)break;if(g_ready){if(e->current_data_offset==0)g_drops++;break;}if(e->current_data_offset==0){reset_rx();size_t tn=(size_t)e->topic_len<sizeof(g_topic)-1?(size_t)e->topic_len:sizeof(g_topic)-1;memcpy(g_topic,e->topic,tn);g_topic[tn]=0;g_rx_need=e->total_data_len;if((size_t)g_rx_need>=MQTT_RX_MAX){g_drop=true;break;}}if(g_drop)break;size_t off=e->current_data_offset,n=e->data_len;if(off+n>=MQTT_RX_MAX){g_drop=true;break;}memcpy(g_rx+off,e->data,n);if(off+n>g_rx_have)g_rx_have=off+n;if(g_rx_have>=(size_t)g_rx_need){g_rx[g_rx_need]=0;g_ready=true;if(g_parser)xTaskNotifyGive(g_parser);}break;}default:break;}}
static bool mqtt_prepare(){static char uri[128];snprintf(uri,sizeof(uri),"mqtt://%s:%d",MQTT_HOST,MQTT_PORT);esp_mqtt_client_config_t c={};c.broker.address.uri=uri;c.credentials.client_id=DEVICE_NAME;if(strlen(MQTT_USERNAME)){c.credentials.username=MQTT_USERNAME;c.credentials.authentication.password=MQTT_PASSWORD;}c.buffer.size=4096;c.buffer.out_size=1024;c.task.stack_size=3072;c.task.priority=5;c.session.keepalive=30;g_mqtt=esp_mqtt_client_init(&c);return g_mqtt&&esp_mqtt_client_register_event(g_mqtt,MQTT_EVENT_ANY,mqtt_event,nullptr)==ESP_OK;}
static void mqtt_start(){if(!g_mqtt||g_mqtt_started)return;if(esp_mqtt_client_start(g_mqtt)!=ESP_OK)return;g_mqtt_started=true;if(!g_parser)xTaskCreate(parser_task,"adsb-parser",4096,nullptr,6,&g_parser);heaplog("after mqtt/parser");}
static void wifi_event(void*,esp_event_base_t b,int32_t id,void*){if(b==WIFI_EVENT&&id==WIFI_EVENT_STA_START)esp_wifi_connect();else if(b==WIFI_EVENT&&id==WIFI_EVENT_STA_DISCONNECTED){g_wifi=false;g_mqtt_ok=false;g_dirty=true;esp_wifi_connect();}else if(b==IP_EVENT&&id==IP_EVENT_STA_GOT_IP){g_wifi=true;g_dirty=true;mqtt_start();}}
static void wifi_init(){ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());ESP_ERROR_CHECK(esp_netif_create_default_wifi_sta()?ESP_OK:ESP_FAIL);wifi_init_config_t c=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&c));ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_event,nullptr));ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,nullptr));wifi_config_t w={};strlcpy((char*)w.sta.ssid,WIFI_SSID,sizeof(w.sta.ssid));strlcpy((char*)w.sta.password,WIFI_PASSWORD,sizeof(w.sta.password));w.sta.threshold.authmode=WIFI_AUTH_WPA2_PSK;ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA,&w));ESP_ERROR_CHECK(esp_wifi_start());}

static lv_obj_t* plain(lv_obj_t*p){auto*o=lv_obj_create(p);lv_obj_remove_style_all(o);return o;}
static lv_obj_t* lab(lv_obj_t*p,const char*t,int y,uint32_t c=C_WHITE){auto*o=lv_label_create(p);lv_label_set_text(o,t);lv_obj_set_style_text_color(o,lv_color_hex(c),0);lv_obj_align(o,LV_ALIGN_TOP_MID,0,y);return o;}
static lv_obj_t* page(lv_obj_t*s){auto*p=plain(s);lv_obj_set_size(p,240,240);lv_obj_align(p,LV_ALIGN_CENTER,0,0);lv_obj_set_style_bg_color(p,lv_color_hex(C_BG),0);lv_obj_set_style_bg_opa(p,LV_OPA_COVER,0);return p;}
static void ring(lv_obj_t*p,int d,uint32_t c,int w=1){auto*r=plain(p);lv_obj_set_size(r,d,d);lv_obj_set_style_radius(r,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_opa(r,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(r,w,0);lv_obj_set_style_border_color(r,lv_color_hex(c),0);lv_obj_align(r,LV_ALIGN_CENTER,0,0);}
static lv_obj_t* line(lv_obj_t*p,uint32_t c,int w=1){auto*l=lv_line_create(p);lv_obj_set_style_line_color(l,lv_color_hex(c),0);lv_obj_set_style_line_width(l,w,0);lv_obj_set_style_line_rounded(l,true,0);return l;}
static bool visible(const Aircraft&a,float range){return a.distance_km>0&&a.distance_km<=range;}
static int pick(const Aircraft*p,size_t n,float range){if(!n)return-1;if(g_selected<0)g_selected=0;int start=g_selected%(int)n;for(size_t k=0;k<n;k++){int i=(start+(int)k)%(int)n;if(visible(p[i],range))return i;}return-1;}
static float adelta(float a,float b){return fabsf(fmodf(a-b+540.0f,360.0f)-180.0f);}

static void focus_cb(lv_event_t*e){int p=(int)(intptr_t)lv_event_get_user_data(e);if(p>=0&&p<5){g_page=p;g_dirty=true;ESP_LOGI(TAG,"page=%d",p);}}
static void click_cb(lv_event_t*e){int p=(int)(intptr_t)lv_event_get_user_data(e);if(g_long){g_long=false;return;}if(p==1||p==2){g_range_idx=(g_range_idx+1)%5;g_dirty=true;}else if(nav_group)lv_group_focus_next(nav_group);}
static void long_cb(lv_event_t*e){int p=(int)(intptr_t)lv_event_get_user_data(e);if(p==1||p==2){g_long=true;g_selected=(g_selected+1)%MAX_PLANES;g_dirty=true;}}

static void build_ui(){auto*s=lv_screen_active();lv_obj_set_style_bg_color(s,lv_color_hex(C_BG),0);lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
 pages[0]=page(s);lab(pages[0],"ADS-B LIVE",12,C_GREEN);ov_count=lab(pages[0],"0 AIRCRAFT",50,C_GREEN);ov_info=lab(pages[0],"POS 0\nMSG/S 0\nMAX 0 km",88,C_WHITE);lv_obj_set_style_text_align(ov_info,LV_TEXT_ALIGN_CENTER,0);ov_near=lab(pages[0],"NEAREST ---",155,C_GREEN2);lv_obj_set_style_text_align(ov_near,LV_TEXT_ALIGN_CENTER,0);
 pages[1]=page(s);ring(pages[1],236,C_GREEN2,2);ring(pages[1],180,C_DIM);ring(pages[1],124,C_DIM);ring(pages[1],68,C_DIM);static lv_point_precise_t hp[2]={{8,120},{232,120}},vp[2]={{120,8},{120,232}};auto*h=line(pages[1],C_DIM);lv_line_set_points(h,hp,2);auto*v=line(pages[1],C_DIM);lv_line_set_points(v,vp,2);for(int i=0;i<10;i++){sweep_line[i]=line(pages[1],i==0?C_GREEN:(i<3?C_GREEN2:C_DIM),i==0?2:1);lv_line_set_points(sweep_line[i],sweep_pts[i],2);lv_obj_set_style_line_opa(sweep_line[i],(lv_opa_t)(240-i*21),0);}for(size_t i=0;i<MAX_PLANES;i++){auto*d=plain(pages[1]);lv_obj_set_size(d,6,6);lv_obj_set_style_radius(d,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(d,lv_color_hex(C_GREEN),0);lv_obj_set_style_bg_opa(d,LV_OPA_COVER,0);lv_obj_add_flag(d,LV_OBJ_FLAG_HIDDEN);ppi_dot[i]=d;}sel_vector=line(pages[1],C_WHITE,1);lv_line_set_points(sel_vector,sel_vec_pts,2);lv_obj_add_flag(sel_vector,LV_OBJ_FLAG_HIDDEN);ppi_title=lab(pages[1],"PPI 50 km",5,C_GREEN);ppi_sel=lab(pages[1],"",181,C_WHITE);lv_obj_set_style_text_align(ppi_sel,LV_TEXT_ALIGN_CENTER,0);ppi_count=lab(pages[1],"0 targets",218,C_GREEN2);for(int i=0;i<3;i++){ring_label[i]=lv_label_create(pages[1]);lv_obj_set_style_text_color(ring_label[i],lv_color_hex(C_DIM),0);}lv_obj_set_pos(ring_label[0],144,123);lv_obj_set_pos(ring_label[1],171,123);lv_obj_set_pos(ring_label[2],199,123);auto*n=lab(pages[1],"N",22,C_GREEN);(void)n;
 pages[2]=page(s);ring(pages[2],236,C_GREEN2,2);ring(pages[2],180,C_DIM);ring(pages[2],124,C_DIM);traffic_title=lab(pages[2],"TRAFFIC 50 km",5,C_GREEN);traffic_sel=lab(pages[2],"",181,C_WHITE);lv_obj_set_style_text_align(traffic_sel,LV_TEXT_ALIGN_CENTER,0);traffic_count=lab(pages[2],"0 tracks",218,C_GREEN2);for(size_t i=0;i<TRAFFIC_PLANES;i++){traffic_plane[i]=line(pages[2],C_GREEN,2);lv_line_set_points(traffic_plane[i],traffic_pts[i],5);lv_obj_add_flag(traffic_plane[i],LV_OBJ_FLAG_HIDDEN);}
 pages[3]=page(s);lab(pages[3],"NEAREST",12,C_GREEN);near_info=lab(pages[3],"---",56,C_WHITE);lv_obj_set_style_text_align(near_info,LV_TEXT_ALIGN_CENTER,0);
 pages[4]=page(s);lab(pages[4],"SYSTEM",12,C_GREEN);sys_info=lab(pages[4],"BOOTING",52,C_WHITE);lv_obj_set_style_text_align(sys_info,LV_TEXT_ALIGN_CENTER,0);
 alert_label=lv_label_create(s);lv_label_set_text(alert_label,"");lv_obj_set_style_text_color(alert_label,lv_color_white(),0);lv_obj_set_style_bg_color(alert_label,lv_color_hex(0xA00000),0);lv_obj_set_style_bg_opa(alert_label,LV_OPA_COVER,0);lv_obj_set_style_pad_all(alert_label,4,0);lv_obj_align(alert_label,LV_ALIGN_BOTTOM_MID,0,-6);lv_obj_add_flag(alert_label,LV_OBJ_FLAG_HIDDEN);
 auto*indev=bsp_display_get_input_dev();if(indev&&lv_indev_get_type(indev)==LV_INDEV_TYPE_ENCODER){nav_group=lv_group_create();for(int i=0;i<5;i++){auto*b=lv_button_create(s);lv_obj_set_size(b,1,1);lv_obj_set_pos(b,-10,-10);lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(b,0,0);lv_obj_set_style_outline_width(b,0,0);lv_obj_add_event_cb(b,focus_cb,LV_EVENT_FOCUSED,(void*)(intptr_t)i);lv_obj_add_event_cb(b,click_cb,LV_EVENT_CLICKED,(void*)(intptr_t)i);lv_obj_add_event_cb(b,long_cb,LV_EVENT_LONG_PRESSED,(void*)(intptr_t)i);lv_group_add_obj(nav_group,b);nav[i]=b;}lv_indev_set_group(indev,nav_group);lv_group_focus_obj(nav[0]);}
 for(int i=1;i<5;i++)lv_obj_add_flag(pages[i],LV_OBJ_FLAG_HIDDEN);
}
static void show_page(int p){for(int i=0;i<5;i++){if(i==p)lv_obj_remove_flag(pages[i],LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(pages[i],LV_OBJ_FLAG_HIDDEN);}}
static void update_sweep(){for(int i=0;i<10;i++){float a=(float)(g_sweep-i*3-90)*PI_F/180.0f;sweep_pts[i][0]={CX,CY};sweep_pts[i][1]={(lv_value_precise_t)(CX+(int)(cosf(a)*RR)),(lv_value_precise_t)(CY+(int)(sinf(a)*RR))};lv_line_set_points(sweep_line[i],sweep_pts[i],2);}for(size_t i=0;i<MAX_PLANES;i++){if(!dot_active[i])continue;if(dot_emergency[i]){lv_obj_set_style_opa(ppi_dot[i],LV_OPA_COVER,0);continue;}float d=adelta((float)g_sweep,dot_bearing[i]);lv_obj_set_style_opa(ppi_dot[i],d<7?LV_OPA_COVER:(d<35?210:145),0);}}
static void plane_shape(size_t i,int x,int y,int track,bool selected){float a=((float)track-90)*PI_F/180.0f;float ca=cosf(a),sa=sinf(a),px=-sa,py=ca;int len=selected?11:8,w=selected?6:5;traffic_pts[i][0]={(lv_value_precise_t)(x-(int)(ca*len/2)),(lv_value_precise_t)(y-(int)(sa*len/2))};traffic_pts[i][1]={(lv_value_precise_t)(x+(int)(ca*len/2)),(lv_value_precise_t)(y+(int)(sa*len/2))};traffic_pts[i][2]={(lv_value_precise_t)(x+(int)(px*w)),(lv_value_precise_t)(y+(int)(py*w))};traffic_pts[i][3]={(lv_value_precise_t)(x-(int)(px*w)),(lv_value_precise_t)(y-(int)(py*w))};traffic_pts[i][4]=traffic_pts[i][0];lv_line_set_points(traffic_plane[i],traffic_pts[i],5);lv_obj_set_style_line_color(traffic_plane[i],lv_color_hex(selected?C_WHITE:C_GREEN),0);lv_obj_set_style_line_width(traffic_plane[i],selected?3:2,0);}
static void update_data(const Summary&s,const Aircraft*p,size_t count){char b[192],id[10],sq[5];strlcpy(id,s.nearest.flight[0]?s.nearest.flight:(s.nearest.hex[0]?s.nearest.hex:"---"),sizeof(id));strlcpy(sq,s.nearest.squawk[0]?s.nearest.squawk:"----",sizeof(sq));snprintf(b,sizeof(b),"%d AIRCRAFT",s.aircraft);lv_label_set_text(ov_count,b);snprintf(b,sizeof(b),"POS %d\nMSG/S %.0f\nMAX %.0f km",s.positioned,(double)s.msg_rate,(double)s.max_range);lv_label_set_text(ov_info,b);snprintf(b,sizeof(b),"NEAREST %s\n%.1f km  %d ft",id,(double)s.nearest.distance_km,s.nearest.altitude_ft);lv_label_set_text(ov_near,b);snprintf(b,sizeof(b),"%s\n%.1f km\n%d ft\n%.0f kt\nHDG %03d\nSQ %s",id,(double)s.nearest.distance_km,s.nearest.altitude_ft,(double)s.nearest.speed_kt,s.nearest.track,sq);lv_label_set_text(near_info,b);
 int64_t n=now_ms();snprintf(b,sizeof(b),"%s\nWi-Fi %s  MQTT %s\nSummary %lds\nAircraft %lds\nDrops %u\nUI: PPI + TRAFFIC",s.online?"SOURCE ONLINE":"SOURCE OFFLINE",g_wifi?"OK":"DOWN",g_mqtt_ok?"OK":"DOWN",g_last_summary?(long)((n-g_last_summary)/1000):-1L,g_last_aircraft?(long)((n-g_last_aircraft)/1000):-1L,(unsigned)g_drops);lv_label_set_text(sys_info,b);
 if(count>MAX_PLANES)count=MAX_PLANES;float range=(float)g_ranges[g_range_idx];int sel=pick(p,count,range);if(sel>=0)g_selected=sel;snprintf(b,sizeof(b),"PPI %d km",(int)range);lv_label_set_text(ppi_title,b);snprintf(b,sizeof(b),"TRAFFIC %d km",(int)range);lv_label_set_text(traffic_title,b);float rv[3]={range*.30f,range*.55f,range*.80f};for(int i=0;i<3;i++){snprintf(b,sizeof(b),rv[i]<10?"%.1f":"%.0f",(double)rv[i]);lv_label_set_text(ring_label[i],b);}size_t shown=0;int sx=CX,sy=CY;for(size_t i=0;i<MAX_PLANES;i++){dot_active[i]=dot_emergency[i]=false;if(i>=count||!visible(p[i],range)){lv_obj_add_flag(ppi_dot[i],LV_OBJ_FLAG_HIDDEN);continue;}float a=(p[i].bearing-90)*PI_F/180.0f,rad=(p[i].distance_km/range)*(RR-12);int x=CX+(int)lroundf(cosf(a)*rad),y=CY+(int)lroundf(sinf(a)*rad);bool em=!strcmp(p[i].squawk,"7500")||!strcmp(p[i].squawk,"7600")||!strcmp(p[i].squawk,"7700"),selected=(int)i==sel;int z=em?9:(selected?9:6);lv_obj_set_size(ppi_dot[i],z,z);lv_obj_set_style_bg_color(ppi_dot[i],lv_color_hex(em?C_RED:(selected?C_WHITE:C_GREEN)),0);lv_obj_set_pos(ppi_dot[i],x-z/2,y-z/2);lv_obj_remove_flag(ppi_dot[i],LV_OBJ_FLAG_HIDDEN);dot_active[i]=true;dot_bearing[i]=p[i].bearing;dot_emergency[i]=em;if(selected){sx=x;sy=y;}shown++;}
 if(sel>=0){auto&q=p[sel];const char*sid=q.flight[0]?q.flight:(q.hex[0]?q.hex:"---");snprintf(b,sizeof(b),"%s  %.1fkm  %dft\n%.0fkt  HDG %03d",sid,(double)q.distance_km,q.altitude_ft,(double)q.speed_kt,q.track);lv_label_set_text(ppi_sel,b);lv_label_set_text(traffic_sel,b);float ta=((float)q.track-90)*PI_F/180.0f;sel_vec_pts[0]={(lv_value_precise_t)sx,(lv_value_precise_t)sy};sel_vec_pts[1]={(lv_value_precise_t)(sx+(int)(cosf(ta)*15)),(lv_value_precise_t)(sy+(int)(sinf(ta)*15))};lv_line_set_points(sel_vector,sel_vec_pts,2);lv_obj_remove_flag(sel_vector,LV_OBJ_FLAG_HIDDEN);}else{lv_label_set_text(ppi_sel,"");lv_label_set_text(traffic_sel,"");lv_obj_add_flag(sel_vector,LV_OBJ_FLAG_HIDDEN);}snprintf(b,sizeof(b),"%u/%u targets",(unsigned)shown,(unsigned)count);lv_label_set_text(ppi_count,b);
 size_t tshown=0;for(size_t i=0;i<TRAFFIC_PLANES;i++){if(i>=count||!visible(p[i],range)){lv_obj_add_flag(traffic_plane[i],LV_OBJ_FLAG_HIDDEN);continue;}float a=(p[i].bearing-90)*PI_F/180.0f,rad=(p[i].distance_km/range)*(RR-18);int x=CX+(int)lroundf(cosf(a)*rad),y=CY+(int)lroundf(sinf(a)*rad);plane_shape(i,x,y,p[i].track,(int)i==sel);lv_obj_remove_flag(traffic_plane[i],LV_OBJ_FLAG_HIDDEN);tshown++;}snprintf(b,sizeof(b),"%u tracks",(unsigned)tshown);lv_label_set_text(traffic_count,b);
 if(g_alert[0]&&now_ms()<g_alert_until){lv_label_set_text(alert_label,g_alert);lv_obj_remove_flag(alert_label,LV_OBJ_FLAG_HIDDEN);}else lv_obj_add_flag(alert_label,LV_OBJ_FLAG_HIDDEN);
}
static void ui_task(void*){Summary s{};Aircraft p[MAX_PLANES]{};size_t count=0;int last=-1;int64_t status=0;for(;;){int64_t n=now_ms();bool timed=n-status>=5000,need=g_dirty||timed;if(need&&xSemaphoreTake(g_mutex,pdMS_TO_TICKS(50))){s=g_summary;count=g_count>MAX_PLANES?MAX_PLANES:g_count;memcpy(p,g_planes,sizeof(p));xSemaphoreGive(g_mutex);g_dirty=false;if(timed)status=n;}bool sw=g_page==1&&n-g_last_sweep>=55,chg=g_page!=last;if(need||sw||chg){if(bsp_display_lock(250)){if(chg){show_page(g_page);last=g_page;}if(need)update_data(s,p,count);if(sw){g_sweep=(g_sweep+4)%360;update_sweep();g_last_sweep=now_ms();}bsp_display_unlock();}}vTaskDelay(pdMS_TO_TICKS(20));}}

extern "C" void app_main(void){ESP_LOGI(TAG,"ADS-B NextGen UI starting");esp_err_t e=nvs_flash_init();if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());e=nvs_flash_init();}ESP_ERROR_CHECK(e);g_mutex=xSemaphoreCreateMutex();ESP_ERROR_CHECK(g_mutex?ESP_OK:ESP_ERR_NO_MEM);if(!mqtt_prepare())ESP_LOGE(TAG,"mqtt prepare failed");auto*d=bsp_display_start();ESP_ERROR_CHECK(d?ESP_OK:ESP_FAIL);ESP_ERROR_CHECK(bsp_display_backlight_on());if(bsp_display_lock(1000)){build_ui();bsp_display_unlock();}xTaskCreate(ui_task,"adsb-ui",5120,nullptr,4,nullptr);wifi_init();}
