idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "." "../include"
  REQUIRES esp_wifi nvs_flash mqtt json esp_event esp_netif driver
)
