
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#include <zephyr/random/random.h>
#include "shared_data.h"

#define BROKER_ADDR CONFIG_LPS_MQTT_BROKER_ADDR 
#define BROKER_PORT CONFIG_LPS_MQTT_BROKER_PORT

// MQTT State buffers
static uint8_t rx_buffer[128];
static uint8_t tx_buffer[128];
static struct mqtt_client client_ctx;
static struct sockaddr_in broker;
static struct zsock_pollfd fds[1];

extern void lps_wifi_prepare_connection(void);
extern void lps_wifi_teardown_connection(void);

lp_shared_data_t *sensor_data;

static void mqtt_evt_handler(struct mqtt_client *const client,
                             const struct mqtt_evt *evt)
{
    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        printk("MQTT connected!\n");
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

static int publish_sensor_data(int16_t temp_c_x10, uint16_t rh_x10)
{
    char payload[128];
    char device_id[20];
    
    get_device_id(device_id, sizeof(device_id));
    
    // Convert your x10 integers to float strings for the JSON payload
    snprintf(payload, sizeof(payload), 
             "{\"id\": \"%s\", \"temperature\": %d.%d, \"humidity\": %d.%d}", 
             device_id, 
             temp_c_x10 / 10, abs(temp_c_x10 % 10), 
             rh_x10 / 10, abs(rh_x10 % 10));

    // Build a unique Topic string using that same ID
    char topic[64];
    snprintf(topic, sizeof(topic), "sensors/%s/env", device_id);
    //snprintf(topic, sizeof(topic), "sensors/greenhouse/env");
    
    struct mqtt_publish_param param;
    param.message.topic.qos = MQTT_QOS_0_AT_MOST_ONCE;
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
    
    mqtt_client_init(&client_ctx);

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

    printk("Connecting to MQTT broker...\n");
    if (mqtt_connect(&client_ctx) != 0) {
        printk("Failed to connect to broker.\n");
        return;
    }

    // Process the connection
    fds[0].fd = client_ctx.transport.tcp.sock;
    fds[0].events = ZSOCK_POLLIN;
    
    // Wait for the CONNACK
    if (zsock_poll(fds, 1, 5000) > 0) {
        mqtt_input(&client_ctx);
    }

    printk("Publishing data...\n");
    publish_sensor_data(sensor_data->temp_c_x10, sensor_data->rh_x10);

    // Let the socket push the data out before killing the connection
    k_msleep(200); 
    mqtt_disconnect(&client_ctx, NULL);
}

void lps_send_update(lp_shared_data_t *data) {

    sensor_data = data;

    lps_wifi_prepare_connection();
    
    send_mqtt_update();

    lps_wifi_teardown_connection();

    // Give the radio hardware 150ms to physically transmit the 
    // packets over the air before the CPU issues the deep sleep halt.
    k_msleep(150);

}
