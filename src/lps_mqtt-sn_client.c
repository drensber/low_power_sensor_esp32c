#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt_sn.h>

#include <zephyr/net/openthread.h>
#include <zephyr/sys/util.h> 
#include <openthread/thread.h>
#include <openthread/ip6.h>

#include "lps_shared.h"


LOG_MODULE_DECLARE(lps_hp, CONFIG_LPS_LOG_LEVEL);

/* Global MQTT-SN Buffers and Structs */
static uint8_t mqtt_sn_tx_buf[512];
static uint8_t mqtt_sn_rx_buf[512];
static struct mqtt_sn_client sn_client;
static struct mqtt_sn_transport_udp sn_transport; /* Zephyr 4.4 Transport Wrapper */

static struct sockaddr_in6 gateway_addr;

static volatile bool is_mqtt_sn_connected = false;

static bool is_mqtt_sn_initialized = false;
static bool is_gateway_connected = false;
static bool is_topic_registered = false;


/* Zephyr 4.4 strictly requires an event callback */
static void mqtt_sn_evt_cb(struct mqtt_sn_client *client, const struct mqtt_sn_evt *evt)
{
    switch (evt->type) {
        case MQTT_SN_EVT_CONNECTED:
            LOG_DBG("MQTT-SN Event: CONNECTED");
	    is_mqtt_sn_connected = true;
            break;
        case MQTT_SN_EVT_DISCONNECTED:
            LOG_DBG("MQTT-SN Event: DISCONNECTED");
	    is_mqtt_sn_connected = false;
            break;
        default:
            break;
    }
}


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

static bool lps_mqtt_sn_register_topic(struct mqtt_sn_client *client, struct mqtt_sn_data *topic) 
{
    LOG_DBG("Initiating Topic Registration for: %s", topic->data);

    /* Zephyr drops the payload during REGISTER, so we feed it an empty dummy struct */
    struct mqtt_sn_data dummy_payload = {0};

    if (mqtt_sn_publish(client, MQTT_SN_QOS_0, topic, false, &dummy_payload) != 0) {
        LOG_ERR("MQTT-SN Register API failed");
        return false;
    }

    struct zsock_pollfd reg_fds[1] = {
        { .fd = sn_transport.sock, .events = ZSOCK_POLLIN, .revents = 0 }
    };

    /* Block until the REGACK arrives */
    int reg_timeout = 15000; /* Give the mesh 15 seconds to converge on first boot */
    while (!is_topic_registered && reg_timeout > 0) {
        if (zsock_poll(reg_fds, 1, 1000) > 0) {
            mqtt_sn_input(client); 
            LOG_DBG("Topic successfully registered!");
            return true;
        }
        reg_timeout -= 1000;
    }

    
    LOG_ERR("Timeout: Gateway never sent REGACK");
    return false;
}

static bool lps_mqtt_sn_publish_payload(struct mqtt_sn_client *client, struct mqtt_sn_data *topic, struct mqtt_sn_data *payload) 
{
    LOG_DBG("Sending the actual PUBLISH packet...");

    /* Because the topic string is now cached, Zephyr swaps it for the ID and fires */
    if (mqtt_sn_publish(client, MQTT_SN_QOS_0, topic, false, payload) != 0) {
        LOG_ERR("MQTT-SN Publish API failed");
        return false;
    }
    
    LOG_DBG("PUBLISH successful!");
    return true;
}

static void pump_mqtt_sn_socket(struct mqtt_sn_transport_udp *transport, 
                                struct mqtt_sn_client *client, 
                                int timeout_ms) 
{
    struct zsock_pollfd fds[1] = {
        { .fd = transport->sock, .events = ZSOCK_POLLIN, .revents = 0 }
    };

    int time_left = timeout_ms;
    while (time_left > 0) {
        if (zsock_poll(fds, 1, 100) > 0) {
            mqtt_sn_input(client);
        }
        time_left -= 100;
    }
}

bool lps_send_update(lp_to_hp_shared_data_t *sensor_data) 
{
    static bool successful_publish = false;
    /* 1. Persistent OS Context */
    static bool is_net_initialized = false;
    
    /* 2. Persistent Payload Memory (Prevents the (null) pointer bug) */
    char device_id[20];
    char payload[128];
    char topic[64];
    
    static struct mqtt_sn_data pub_data;
    static struct mqtt_sn_data pub_topic;
    
#if 0    
    static struct mqtt_sn_data topic = MQTT_SN_DATA_STRING_LITERAL("sensors/esp32c6/env");

    snprintf(payload, sizeof(payload), 
             "{\"temp\": %d.%d, \"hum\": %d.%d}", 
             sensor_data->temp_c_x10 / 10, abs(sensor_data->temp_c_x10 % 10),
             sensor_data->rh_x10 / 10, abs(sensor_data->rh_x10 % 10));
#endif

    get_device_id(device_id, sizeof(device_id));
    get_json_message(payload, sizeof(payload), device_id, sensor_data);
    get_topic(topic, sizeof(topic), device_id);
    
    pub_data.data = (uint8_t *)payload;
    pub_data.size = strlen(payload);

    pub_topic.data = (uint8_t *)topic;
    pub_topic.size = strlen(topic);

    /* 3. STRICTLY One-Time Initialization */
    if (!is_net_initialized) {
        LOG_DBG("--- Initializing MQTT-SN OS Structures ---");
        
        struct sockaddr_in6 mcast_addr = {0};
        mcast_addr.sin6_family = AF_INET6;
        mcast_addr.sin6_port = htons(0); 
        zsock_inet_pton(AF_INET6, "ff03::1", &mcast_addr.sin6_addr);

        if (mqtt_sn_transport_udp_init(&sn_transport, (struct sockaddr *)&mcast_addr, sizeof(mcast_addr)) < 0) {
            return false;
        }

        struct mqtt_sn_data client_id = MQTT_SN_DATA_STRING_LITERAL("esp32c6_sensor");
        if (mqtt_sn_client_init(&sn_client, &client_id, &sn_transport.tp, mqtt_sn_evt_cb, 
                                mqtt_sn_tx_buf, sizeof(mqtt_sn_tx_buf), 
                                mqtt_sn_rx_buf, sizeof(mqtt_sn_rx_buf)) < 0) {
            return false;
        }

        struct sockaddr_in6 gw_addr = {0};
        gw_addr.sin6_family = AF_INET6;
        gw_addr.sin6_port = htons(CONFIG_LPS_MQTT_BROKER_PORT);
        zsock_inet_pton(AF_INET6, CONFIG_LPS_MQTT_BROKER_ADDR, &gw_addr.sin6_addr);
        
        struct mqtt_sn_data gw_addr_data = { .data = (const uint8_t *)&gw_addr, .size = sizeof(gw_addr) };
        mqtt_sn_add_gw(&sn_client, 1, gw_addr_data);

        is_net_initialized = true;
    }

    LOG_DBG("Waking up. Waiting for Thread Mesh...");
    wait_for_thread_mesh(30000);
    
    /* 4. Speed up MAC layer for the transaction */
    otInstance *ot = openthread_get_default_instance();
    if (ot != NULL) otLinkSetPollPeriod(ot, 100);

    /* 5. Connect */
    LOG_DBG("--- Connecting ---");
    int retries = 3;
    is_mqtt_sn_connected = false;

    while (retries > 0 && !is_mqtt_sn_connected) {
        LOG_DBG("Sending MQTT-SN CONNECT (Attempt %d)...", 4 - retries);
        
        if (mqtt_sn_connect(&sn_client, false, true) != 0) {
            LOG_ERR("MQTT-SN Connect API failed");
            if (ot != NULL) otLinkSetPollPeriod(ot, 0);
            return false;
        }

        /* Pump the socket for up to 3 seconds waiting for the CONNACK */
        pump_mqtt_sn_socket(&sn_transport, &sn_client, 3000);

        if (!is_mqtt_sn_connected) {
            LOG_WRN("Timeout. Mesh likely dropped packet for Route Discovery. Retrying...");
            retries--;
        }
    }

    if (!is_mqtt_sn_connected) {
        LOG_ERR("Failed to connect after retries.");
        if (ot != NULL) otLinkSetPollPeriod(ot, 0);
        return false;
    }


    /* 6. Register & Publish */
    LOG_DBG("--- Registering & Publishing ---");
    
    /* Call 1: Sends REGISTER packet and caches the string internally */
    mqtt_sn_publish(&sn_client, MQTT_SN_QOS_0, &pub_topic, false, &pub_data);
    
    /* Pump the socket for up to 5 seconds to catch the REGACK */
    pump_mqtt_sn_socket(&sn_transport, &sn_client, 5000);
    
    /* Call 2: Uses the newly cached ID to send the actual PUBLISH packet */
    if (mqtt_sn_publish(&sn_client, MQTT_SN_QOS_0, &pub_topic, false, &pub_data) == 0) {
        LOG_DBG("Publish dispatched!");
        successful_publish = true;
    }

    /* 7. Clean Disconnect */
    LOG_DBG("--- Disconnecting ---");
    mqtt_sn_disconnect(&sn_client);
    
    /* Give the radio 500ms to physically transmit the DISCONNECT packet */
    pump_mqtt_sn_socket(&sn_transport, &sn_client, 500);

    /* Drop MAC poll to save power */
    if (ot != NULL) otLinkSetPollPeriod(ot, 0);

    return successful_publish;
}
