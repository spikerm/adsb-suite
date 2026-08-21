idf_component_register(
  SRCS "main.cpp"
  INCLUDE_DIRS "." "../include"
  REQUIRES esp32_c3_lcdkit esp_wifi nvs_flash mqtt json esp_event esp_netif esp_timer driver
)

# Compile-time shim that only enlarges the task named "adsb-ui".
# Keep -include and its path in one compiler argument; passing them as two
# CMake list elements caused GCC to treat the header path as a second input file.
target_compile_options(${COMPONENT_LIB} PRIVATE
  "-include${CMAKE_CURRENT_SOURCE_DIR}/ui_stack_fix.h"
)
