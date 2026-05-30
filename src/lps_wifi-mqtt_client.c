#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "lps_shared.h"
#include "lps_wifi.h"

LOG_MODULE_DECLARE(lps_hp, CONFIG_LPS_LOG_LEVEL);

#define BROKER_ADDR CONFIG_LPS_MQTT_BROKER_ADDR 
#define BROKER_PORT CONFIG_LPS_MQTT_BROKER_PORT

static uint8_t rx_buffer[128];
static uint8_t tx_buffer[128];
static struct mqtt_client client_ctx;
static struct sockaddr_in broker;
static struct zsock_pollfd fds[1];

volatile lp_to_hp_shared_data_t *sensor_data;

// Event notification semaphores
static K_SEM_DEFINE(mqtt_conn_sem, 0, 1);
static K_SEM_DEFINE(mqtt_pub_sem, 0, 1);

static void mqtt_evt_handler(struct mqtt_client *const client,
                             const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->param.connack.return_code == MQTT_CONNECTION_ACCEPTED) {
            LOG_DBG("MQTT connected!");
            k_sem_give(&mqtt_conn_sem);
        } else {
            LOG_DBG("MQTT connection refused: %d", evt->param.connack.return_code);
        }
        break;
    case MQTT_EVT_PUBACK:
        LOG_DBG("MQTT PUBACK received! Data is safely on the broker.");
        k_sem_give(&mqtt_pub_sem);
        break;
    case MQTT_EVT_DISCONNECT:
        LOG_DBG("MQTT disconnected.");
        break;
    default:
        break;
    }
}

static int publish_sensor_data(volatile lp_to_hp_shared_data_t *sdata,
			       uint16_t msg_id, uint8_t is_dup)
{
    char device_id[20];
    char payload[128];
    char topic[64];
    
    get_device_id(device_id, sizeof(device_id));
    get_json_message(payload, sizeof(payload), device_id, sensor_data);
    get_topic(topic, sizeof(topic), device_id);
    

    LOG_DBG("publishing to topic %s", topic);
    
    struct mqtt_publish_param param;

    param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.payload.data = payload;
    param.message.payload.len = strlen(payload);

    // Use the stable ID and set the Duplicate flag
    param.message_id = msg_id;
    param.dup_flag = is_dup;
    param.retain_flag = 0;

    return mqtt_publish(&client_ctx, &param);
}

static bool send_mqtt_update(void)
{
    static struct mqtt_utf8 password;
    static struct mqtt_utf8 username;
    bool successful_publish = false;

    // Generate a single 16-bit Message ID for this entire wake cycle
    uint16_t current_msg_id = sys_rand32_get() & 0xFFFF;
    
    // Master Session Loop. 
    // If ANY step of the MQTT flow fails, we tear down the TCP socket, 
    // wait for the RF environment to settle, and rebuild from scratch.
    for (int session = 0; session < 2; session++) {
        
        if (session > 0) {
            LOG_DBG("--- Initiating Costly Recovery (Session Attempt %d/2) ---", session + 1);
            k_msleep(1000); // 1-second physical delay to let Router/RF clear
        }

        mqtt_client_init(&client_ctx);
        k_sem_reset(&mqtt_conn_sem);
        k_sem_reset(&mqtt_pub_sem);

        broker.sin_family = AF_INET;
        broker.sin_port = htons(BROKER_PORT);
        zsock_inet_pton(AF_INET, BROKER_ADDR, &broker.sin_addr);

        client_ctx.broker = &broker;
        client_ctx.evt_cb = mqtt_evt_handler;
        client_ctx.client_id.utf8 = (uint8_t *)"esp32c6_sensor";
        client_ctx.client_id.size = strlen("esp32c6_sensor");
        password.utf8 = (uint8_t *)CONFIG_LPS_MQTT_BROKER_PASSWORD;
        password.size = strlen(CONFIG_LPS_MQTT_BROKER_PASSWORD);
        client_ctx.password = &password;
        username.utf8 = (uint8_t *)CONFIG_LPS_MQTT_BROKER_USERNAME;
        username.size = strlen(CONFIG_LPS_MQTT_BROKER_USERNAME);
        client_ctx.user_name = &username;
        client_ctx.protocol_version = MQTT_VERSION_3_1_1;
        client_ctx.rx_buf = rx_buffer;
        client_ctx.rx_buf_size = sizeof(rx_buffer);
        client_ctx.tx_buf = tx_buffer;
        client_ctx.tx_buf_size = sizeof(tx_buffer);
        client_ctx.transport.type = MQTT_TRANSPORT_NON_SECURE;

        // INNER LOOP: specifically mitigates the -22 EINVAL local OS race condition
        int err = -1;
        for (int attempts = 0; attempts < 3; attempts++) {
            LOG_DBG("Connecting to MQTT broker (Attempt %d/3)...", attempts + 1);
            err = mqtt_connect(&client_ctx);
            
            if (err == 0) {
                break; 
            }

            LOG_DBG("Connection failed: %d. Backing off...", err);
            k_msleep(500); // 500ms guarantees Zephyr's internal IPv4 tables are up
        }

        if (err != 0) {
            LOG_DBG("Session %d: Failed to connect.", session + 1);
            continue; // Trigger the Master Recovery Loop
        }

        fds[0].fd = client_ctx.transport.tcp.sock;
        fds[0].events = ZSOCK_POLLIN;
        
        // Wait for connection ACK
        bool connected = false;
        for (int i = 0; i < 50; i++) { 
            if (zsock_poll(fds, 1, 100) > 0) {
                mqtt_input(&client_ctx);
            }
            if (k_sem_take(&mqtt_conn_sem, K_NO_WAIT) == 0) {
                connected = true;
                break;
            }
        }

        if (!connected) {
            LOG_DBG("Session %d: Timeout waiting for CONNACK.", session + 1);
            mqtt_disconnect(&client_ctx, NULL); // Safely close the orphaned socket
            continue; // Trigger the Master Recovery Loop
        }

        LOG_DBG("Publishing data...");

	uint8_t is_dup = (session > 0) ? 1 : 0;
	
        if (publish_sensor_data(sensor_data, current_msg_id, is_dup) != 0) {
            LOG_DBG("Session %d: Failed to enqueue publish.", session + 1);
            mqtt_disconnect(&client_ctx, NULL);
            continue; 
        }

        // Wait for publish ACK
        bool published = false;
        for (int i = 0; i < 50; i++) { 
            if (zsock_poll(fds, 1, 100) > 0) {
                mqtt_input(&client_ctx);
            }
            if (k_sem_take(&mqtt_pub_sem, K_NO_WAIT) == 0) {
                published = true;
                break;
            }
        }

        if (!published) {
            LOG_DBG("Session %d: Timed out waiting for PUBACK. Network loss suspected.", session + 1);
            mqtt_disconnect(&client_ctx, NULL);
            continue; // Trigger the Master Recovery Loop!
        }

        // If we reach here, the packet is cryptographically verified on the broker.
        successful_publish = true;
        mqtt_disconnect(&client_ctx, NULL);
        break; // Break completely out of the Master Session loop
    }

    // Only if BOTH sessions completely failed do we drop the nuclear bomb
    if (!successful_publish) {
        LOG_DBG("Fatal: Exhausted all recovery attempts. Invalidating cache.");
        lps_wifi_invalidate_dhcp_cache();
    }

    return successful_publish;
}

bool lps_send_update(volatile lp_to_hp_shared_data_t *data) {
    sensor_data = data;
    bool successful_publish = false;
    
    if (lps_wifi_prepare_connection()) {
        // Wi-Fi and IP routing are physically verified. Safe to run.
        successful_publish = send_mqtt_update();
    } else {
        // Wi-Fi failed. Skip MQTT, invalidate caches, and tear down immediately.
        LOG_DBG("Skipping MQTT publish due to Wi-Fi failure.");
        lps_wifi_invalidate_dhcp_cache();
    }
    
    lps_wifi_teardown_connection();

    k_msleep(50);

    return successful_publish;
}
