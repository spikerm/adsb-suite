idf_component_register(
  SRCS "main_nextgen.cpp"
  INCLUDE_DIRS "." "../include"
  REQUIRES esp32_c3_lcdkit esp_wifi nvs_flash mqtt json esp_event esp_netif esp_timer driver
)
