#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "shared_data.h"

LOG_MODULE_DECLARE(lps_hp, CONFIG_LPS_LOG_LEVEL);

bool lps_transport_send_update(lp_to_hp_shared_data_t *data) {
    LOG_DBG("Calling fake lps_transport_send_update()");
    
    k_msleep(2000);
    
    if(data->hp_wake_count > 0 &&
       (data->hp_wake_count % 5 == 0)) {
	LOG_DBG("Returning false from lps_transport_send_update()");
	return false;
    }
    else {
	LOG_DBG("Returning true from lps_transport_send_update()");
	return true;
    }
}
