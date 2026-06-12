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
static struct mqtt_sn_transport_udp sn_transport;

static volatile bool is_mqtt_sn_connected = false;
volatile lp_to_hp_shared_data_t *sensor_data;

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


/* Blocks until the OpenThread stack successfully attaches to the mesh.
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

bool lps_transport_init()
{
    //settings_load();
    return true;
}

bool lps_transport_shutdown()
{
    // Force Zephyr to commit OpenThread's dirty frame counters to NVS
    //settings_save();
    
    /* Give OpenThread's background tasklet time to flush the incremented 
       MAC frame counters to the physical NVS flash. */
    //k_sleep(K_MSEC(500));

    return true;
}

bool lps_transport_send_update(volatile lp_to_hp_shared_data_t *sensor_data) 
{
    static bool successful_publish = false;
    /* Persistent OS Context */
    static bool is_net_initialized = false;
    
    /* Persistent Payload Memory (Prevents the (null) pointer bug) */
    char device_id[20];
    char payload[128];
    char topic[64];
    
    static struct mqtt_sn_data pub_data;
    static struct mqtt_sn_data pub_topic;
    
    get_device_id(device_id, sizeof(device_id));
    get_json_message(payload, sizeof(payload), device_id, sensor_data);
    get_topic(topic, sizeof(topic), device_id);
    
    pub_data.data = (uint8_t *)payload;
    pub_data.size = strlen(payload);

    pub_topic.data = (uint8_t *)topic;
    pub_topic.size = strlen(topic);

    /* One-Time Initialization */
    if (!is_net_initialized) {
        /* OS Initialization & State Flushing */
	static bool is_socket_bound = false;

	if (!is_socket_bound) {
	    LOG_DBG("--- Binding UDP Socket ---");
	    struct sockaddr_in6 mcast_addr = {0};
	    mcast_addr.sin6_family = AF_INET6;
	    mcast_addr.sin6_port = htons(0); 
	    zsock_inet_pton(AF_INET6, "ff03::1", &mcast_addr.sin6_addr);

	    if (mqtt_sn_transport_udp_init(&sn_transport, (struct sockaddr *)&mcast_addr, sizeof(mcast_addr)) < 0) {
		return false;
	    }
	    is_socket_bound = true;
	}

	/* Always re-init the client to flush Zephyr's dirty TX queues */
	LOG_DBG("Flushing MQTT-SN Client state...");
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

    otInstance *ot = openthread_get_default_instance();

    if (ot != NULL ) {
        /* Tell the Border Router to purge this child from its table 
           if it goes completely silent for 15 seconds. */
        otThreadSetChildTimeout(ot, 15);
    }

    LOG_DBG("Waking up. Waiting for Thread Mesh...");
    wait_for_thread_mesh(30000);
    
    /* Speed up MAC layer for the transaction */
    // TODO: Need to figure out which of these is optimal
    //if (ot != NULL) otLinkSetPollPeriod(ot, 250);
    if (ot != NULL) {
	otLinkSetPollPeriod(ot, 100);
    }

    /* Connect */
    LOG_DBG("--- Connecting ---");
    int retries = 3;
    is_mqtt_sn_connected = false;

    while (retries > 0 && !is_mqtt_sn_connected) {
        LOG_DBG("Sending MQTT-SN CONNECT (Attempt %d)...", 4 - retries);

        int err = mqtt_sn_connect(&sn_client, false, true);
        if (err != 0) {
            LOG_WRN("Connect API returned %d. Mesh routing likely converging. Waiting...", err);
            k_msleep(1000); /* Give Thread 1 second to settle its IPv6 routes */
            retries--;
            continue;
        }

        /* Pump the socket for up to 3 seconds waiting for the CONNACK */
        pump_mqtt_sn_socket(&sn_transport, &sn_client, 3000);

        if (!is_mqtt_sn_connected) {
            LOG_WRN("Timeout waiting for CONNACK. Retrying...");
            retries--;
        }
    }

    if (!is_mqtt_sn_connected) {
        LOG_ERR("Failed to connect after retries.");
        if (ot != NULL) otLinkSetPollPeriod(ot, 0);
        return false;
    }


    /* Register & Publish with QoS 1 Handshake Confirmation */
    LOG_DBG("--- Registering & Publishing (QoS 1) ---");
    
    /* Upgrade to QoS 1 so the library tracks the transaction and mandates a PUBACK */
    if (mqtt_sn_publish(&sn_client, MQTT_SN_QOS_1, &pub_topic, false, &pub_data) != 0) {
        LOG_ERR("Publish API failed.");
        successful_publish = false;
    } else {
        /* Poll the socket and wait for the internal tracking queue to empty,
           proving Zephyr successfully intercepted and processed the incoming PUBACK */
        int ack_timeout_ms = 20000;
        successful_publish = false;

        while (ack_timeout_ms > 0) {
            /* Feed incoming datagrams to the state machine */
            pump_mqtt_sn_socket(&sn_transport, &sn_client, 250);
            
            /* Zephyr tracks pending QoS 1/2 acknowledgments in the client.publish list */
            if (sys_slist_is_empty(&sn_client.publish)) {
                LOG_DBG("PUBACK received! Publish safely confirmed by Gateway.");
                successful_publish = true;
                break;
            }
            ack_timeout_ms -= 250;
        }

        if (!successful_publish) {
            LOG_ERR("Publish transaction failed: Timeout waiting for Gateway PUBACK.");
        }
    }

    /* Clean Disconnect */
    LOG_DBG("--- Disconnecting ---");
    mqtt_sn_disconnect(&sn_client);
    
    /* Give the radio 500ms to physically transmit the DISCONNECT packet */
    pump_mqtt_sn_socket(&sn_transport, &sn_client, 500);

    /* Drop MAC poll to save power */
    if (ot != NULL) {
        otLinkSetPollPeriod(ot, 0);
    }

    return successful_publish;
}
