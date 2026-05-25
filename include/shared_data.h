#ifndef SHARED_DATA_H
#define SHARED_DATA_H

#include <stdint.h>
#include <stdbool.h>

#define SHARED_DATA_MAGIC_NUMBER 0x08675309

typedef enum {
    PUBLISH_STATUS_PENDING,
    PUBLISH_STATUS_SUCCESS,
    PUBLISH_STATUS_FAILURE
} publish_status_t;

typedef struct __attribute__((aligned(4))) {
    uint32_t shared_magic;
    uint32_t lp_wake_count;
    uint32_t hp_wake_count;
    int16_t  temp_c_x10; // e.g., 218 = 21.8 C
    uint16_t rh_x10;     // e.g., 452 = 45.2 % RH
    volatile publish_status_t *most_recent_publish_status_p;
} lp_to_hp_shared_data_t;
    
#endif
