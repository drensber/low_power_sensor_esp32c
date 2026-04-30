#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "shared_data.h"

LOG_MODULE_DECLARE(lps_hp, CONFIG_LPS_LOG_LEVEL);

lp_to_hp_shared_data_t *sensor_data;

bool lps_send_update(lp_to_hp_shared_data_t *data) {
    sensor_data = data;
    bool successful_publish = false;

    LOG_DBG("Connecting to MQTT-SN client at \"%s\", port %d",
	    CONFIG_LPS_MQTT_BROKER_ADDR,
	    CONFIG_LPS_MQTT_BROKER_PORT);
    LOG_DBG("Trying to send values temp:%d and hum:%d via Thread/MQTT",
	    data->temp_c_x10, data->rh_x10);
    
    return successful_publish;
}
