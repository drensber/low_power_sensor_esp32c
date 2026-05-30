#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include "shared_data.h"

extern void get_device_id(char *id_buffer, size_t buffer_size);
extern void get_json_message(char *json_message_buffer,
			     size_t buffer_size,
			     char* device_id,
			     volatile lp_to_hp_shared_data_t *data);
extern void get_topic(char *topic_buffer,
		      size_t topic_buffer_size,
		      char *device_id);
