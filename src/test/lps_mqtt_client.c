#include <stdio.h>
#include <zephyr/kernel.h>
#include "shared_data.h"

bool lps_send_update(lp_to_hp_shared_data_t *data) {
    printf("Calling fake lps_send_update()\n");
    
    k_msleep(2000);
    
    if(data->hp_wake_count > 0 &&
       (data->hp_wake_count % 5 == 0)) {
	printf("Returning false from lps_send_update()\n");
	return false;
    }
    else {
	printf("Returning true from lps_send_update()\n");
	return true;
    }
}
