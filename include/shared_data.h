#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <stdint.h>
#include <stdbool.h>

typedef struct __attribute__((aligned(4))) {
    uint32_t lp_wake_count;
    uint32_t hp_wake_count;
    int16_t  temp_c_x10; // e.g., 218 = 21.8 C
    uint16_t rh_x10;     // e.g., 452 = 45.2 % RH    
} lp_shared_data_t;

#endif
