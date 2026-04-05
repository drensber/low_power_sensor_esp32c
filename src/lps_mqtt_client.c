#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include <zephyr/kernel.h>
#include "shared_data.h"
#include "lps_wifi.h"

#define BROKER_ADDR CONFIG_LPS_MQTT_BROKER_ADDR 
#define BROKER_PORT CONFIG_LPS_MQTT_BROKER_PORT

static uint8_t rx_buffer[128];
static uint8_t tx_buffer[128];
static struct mqtt_client client_ctx;
static struct sockaddr_in broker;
static struct zsock_pollfd fds[1];

lp_shared_data_t *sensor_data;

// Event notification semaphores
static K_SEM_DEFINE(mqtt_conn_sem, 0, 1);
static K_SEM_DEFINE(mqtt_pub_sem, 0, 1);

static void mqtt_evt_handler(struct mqtt_client *const client,
                             const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->param.connack.return_code == MQTT_CONNECTION_ACCEPTED) {
            printk("MQTT connected!\n");
            k_sem_give(&mqtt_conn_sem);
        } else {
            printk("MQTT connection refused: %d\n", evt->param.connack.return_code);
        }
        break;
    case MQTT_EVT_PUBACK:
        printk("MQTT PUBACK received! Data is safely on the broker.\n");
        k_sem_give(&mqtt_pub_sem);
        break;
    case MQTT_EVT_DISCONNECT:
        printk("MQTT disconnected.\n");
        break;
    default:
        break;
    }
}

static void get_device_id(char *id_buffer, size_t buffer_size)
{
    struct net_if *iface = net_if_get_default();
    struct net_linkaddr *link_addr = net_if_get_link_addr(iface);

    if (link_addr && link_addr->len == 6) {
        snprintf(id_buffer, buffer_size, "lps_%02x%02x%02x",
                 link_addr->addr[3], link_addr->addr[4], link_addr->addr[5]);
    } else {
        snprintf(id_buffer, buffer_size, "unknown_device");
    }
}

static int publish_sensor_data(int32_t seq, int16_t temp_c_x10, uint16_t rh_x10)
{
    char payload[128];
    char device_id[20];
    
    get_device_id(device_id, sizeof(device_id));
    
    snprintf(payload, sizeof(payload), 
             "{\"id\": \"%s\", \"seq\": %d, \"temperature\": %d.%d, \"humidity\": %d.%d}", 
             device_id,
	     seq,
             temp_c_x10 / 10, abs(temp_c_x10 % 10), 
             rh_x10 / 10, abs(rh_x10 % 10));

    char topic[64];
    snprintf(topic, sizeof(topic), "sensors/%s/env", device_id);
    printf("publishing to topic %s\n", topic);
    
    struct mqtt_publish_param param;

    param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
    param.message.topic.topic.utf8 = (uint8_t *)topic;
    param.message.topic.topic.size = strlen(topic);
    param.message.payload.data = payload;
    param.message.payload.len = strlen(payload);
    param.message_id = sys_rand32_get();
    param.dup_flag = 0;
    param.retain_flag = 0;

    return mqtt_publish(&client_ctx, &param);
}

static void send_mqtt_update(void)
{
    static struct mqtt_utf8 password;
    static struct mqtt_utf8 username;
    int err = -1;

    // 2-Attempt Loop. If the first fails, the ARP hole is 
    // usually fixed by the OS in the background. The second will succeed.
    for (int attempts = 0; attempts < 2; attempts++) {
        
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

        printk("Connecting to MQTT broker (Attempt %d/2)...\n", attempts + 1);
        err = mqtt_connect(&client_ctx);
        
        if (err == 0) {
            break; // Success! Break out of the retry loop.
        }

        printk("Connection failed: %d. Letting ARP settle before retry...\n", err);
        // Give the router an extra 200ms to finish processing Zephyr's 
        // background ARP reply before we fire the second TCP SYN.
        k_msleep(200); 
    }

    // If we exhausted both attempts, the network is truly unreachable.
    if (err != 0) {
        printk("Fatal: Failed to connect after retries. Invalidating cache.\n");
        lps_wifi_invalidate_dhcp_cache();
        return;
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
        printk("Timeout waiting for CONNACK. Invalidating cache.\n");
        lps_wifi_invalidate_dhcp_cache();
        return;
    }

    printk("Publishing data...\n");
    if (publish_sensor_data(sensor_data->hp_wake_count, sensor_data->temp_c_x10, sensor_data->rh_x10) != 0) {
        printk("Failed to enqueue publish.\n");
        return;
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
        printk("Warning: Timed out waiting for PUBACK. Network loss suspected.\n");
    }

    mqtt_disconnect(&client_ctx, NULL);
}

void lps_send_update(lp_shared_data_t *data) {
    sensor_data = data;
    lps_wifi_prepare_connection();
    send_mqtt_update();
    lps_wifi_teardown_connection();
    k_msleep(50); 
}
