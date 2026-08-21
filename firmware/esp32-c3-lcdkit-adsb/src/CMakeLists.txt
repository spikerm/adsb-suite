idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "." "../include"
  REQUIRES esp32_c3_lcdkit esp_wifi nvs_flash mqtt json esp_event esp_netif esp_timer driver
)

# Apply the task-stack shim only while compiling this component.  The MQTT and
# parser task sizes remain untouched; only the task named "adsb-ui" is enlarged.
target_compile_options(${COMPONENT_LIB} PRIVATE
  "-include"
  "${CMAKE_CURRENT_SOURCE_DIR}/ui_stack_fix.h"
)
