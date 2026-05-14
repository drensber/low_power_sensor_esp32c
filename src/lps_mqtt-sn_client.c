#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/net_if.h>

#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include "shared_data.h"

#include <openthread/ip6.h>

LOG_MODULE_DECLARE(lps_hp, CONFIG_LPS_LOG_LEVEL);

lp_to_hp_shared_data_t *sensor_data;
static bool open_thread_configured=false;


/* * Blocks until the OpenThread stack successfully attaches to the mesh.
 * Returns true if attached, false if it times out.
 */
static bool wait_for_thread_mesh(uint32_t timeout_ms) {
    otInstance *instance = openthread_get_default_instance();
    if (!instance) {
        LOG_ERR("OpenThread instance not initialized.");
        return false;
    }

    LOG_DBG("Waiting for OpenThread mesh attachment...");
    
    uint32_t elapsed = 0;
    while (elapsed < timeout_ms) {
        otDeviceRole role = otThreadGetDeviceRole(instance);
        
        /* A Sleepy End Device will attach as a CHILD */
        if (role == OT_DEVICE_ROLE_CHILD || 
            role == OT_DEVICE_ROLE_ROUTER || 
            role == OT_DEVICE_ROLE_LEADER) {
            LOG_DBG("Attached to mesh! Role: %d", role);
            return true;
        }
        
        k_msleep(100);
        elapsed += 100;
    }
    
    LOG_DBG("Timeout waiting for mesh attachment.");
    return false;
}

bool lps_send_update(lp_to_hp_shared_data_t *data) {
    sensor_data = data;
    bool successful_publish = false;

    LOG_DBG("Connecting to MQTT-SN client at \"%s\", port %d",
            CONFIG_LPS_MQTT_BROKER_ADDR,
            CONFIG_LPS_MQTT_BROKER_PORT);
    LOG_DBG("Trying to send values temp:%d and hum:%d via Thread/MQTT-SN",
            data->temp_c_x10, data->rh_x10);



// Only do this once in ALWAYS_STAY_AWAKE configuration    

    if (!open_thread_configured) {

	printk("\n>>> NVS: Loading Thread settings...\n");

#ifdef CONFIG_SETTINGS
	settings_load(); /* 1. Load the keys into OpenThread's RAM */
#endif

	open_thread_configured = true;

    }
    
    LOG_DBG("Waking up. Waiting for Thread Mesh...");
    
    /* Wait up to 10 seconds for the Thread mesh to attach */
    if (!wait_for_thread_mesh(30000)) {
        LOG_DBG("Failed to join Thread network. Aborting publish.");
        k_msleep(100); /* UART flush */
        return false;
    }

    /* Phase 2 & 3: Socket and MQTT-SN logic will go here */

    k_msleep(100); /* UART flush before deep sleep */
    return successful_publish;
}
