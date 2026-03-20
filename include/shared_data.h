#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <stdint.h>
#include <stdbool.h>

typedef struct __attribute__((aligned(4))) {
    uint32_t lp_wake_count;
    uint32_t last_sensor_value;
} lp_shared_data_t;

#endif
