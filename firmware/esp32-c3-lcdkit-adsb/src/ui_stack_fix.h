#pragma once

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * The richer PPI view (selected target, vector and range labels) increased the
 * call depth of update_ui_data().  Keep MQTT/parser task sizes unchanged and
 * only give the ADS-B UI task extra headroom.
 */
static inline BaseType_t adsb_xTaskCreate(TaskFunction_t task_code,
                                          const char *name,
                                          configSTACK_DEPTH_TYPE stack_depth,
                                          void *parameters,
                                          UBaseType_t priority,
                                          TaskHandle_t *created_task)
{
    configSTACK_DEPTH_TYPE adjusted_depth = stack_depth;
    if (name && strcmp(name, "adsb-ui") == 0 && adjusted_depth < 5120) {
        adjusted_depth = 5120;
    }
    return xTaskCreate(task_code, name, adjusted_depth, parameters, priority, created_task);
}

#define xTaskCreate(...) adsb_xTaskCreate(__VA_ARGS__)
