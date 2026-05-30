#include "lps_shared.h"

void get_device_id(char *id_buffer, size_t buffer_size)
{
    struct net_if *iface = net_if_get_default();
    struct net_linkaddr *link_addr = net_if_get_link_addr(iface);

#if defined(CONFIG_LPS_TRANSPORT_WIFI_MQTT)    
    if (link_addr) {
        snprintf(id_buffer, buffer_size, "lps_%02x%02x%02x",
                 link_addr->addr[3], link_addr->addr[4], link_addr->addr[5]);
    }
#elif defined(CONFIG_LPS_TRANSPORT_THREAD_MQTTSN)     
    if (link_addr) {
        snprintf(id_buffer, buffer_size, "lps_%02x%02x%02x%02x",
                 link_addr->addr[4], link_addr->addr[5],
		 link_addr->addr[6], link_addr->addr[7]);
    }
#endif    
    else {
        snprintf(id_buffer, buffer_size, "unknown_device");
    }
}

void get_json_message(char *json_message_buffer, size_t buffer_size,
		      char* device_id, volatile lp_to_hp_shared_data_t *data)
{
    snprintf(json_message_buffer, buffer_size, 
             "{\"id\": \"%s\", \"seq\": %d, \"uptime\": %d, \"temperature\": %d.%d, \"humidity\": %d.%d}", 
             device_id,
	     data->hp_wake_count,
	     data->lp_wake_count,
             (data->temp_c_x10) / 10, abs((data->temp_c_x10) % 10), 
             (data->rh_x10) / 10, abs((data->rh_x10) % 10));    
}

void get_topic(char *topic_buffer, size_t topic_buffer_size, char *device_id)
{
    snprintf(topic_buffer, topic_buffer_size, "sensors/%s/env", device_id);
}
