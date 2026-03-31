#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#include <esp_attr.h>

static bool wifi_connection_is_alive=false;

static RTC_DATA_ATTR bool rtc_has_cached_ap;
static RTC_DATA_ATTR uint8_t rtc_cached_bssid[WIFI_MAC_ADDR_LEN];
static RTC_DATA_ATTR uint8_t rtc_cached_channel;

static K_SEM_DEFINE(wifi_connected, 0, 1);
static struct net_mgmt_event_callback wifi_cb;

static void apply_static_ip_config(void)
{
    struct net_if *iface = net_if_get_default();
    struct in_addr addr;

    // Assign IP Address
    if (net_addr_pton(AF_INET, CONFIG_LPS_IP_ADDR, &addr) == 0) {
        net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
    } else {
        printk("Error parsing IP address.\n");
    }

    // Assign Netmask
    if (net_addr_pton(AF_INET, CONFIG_LPS_NETMASK, &addr) == 0) {
        net_if_ipv4_set_netmask(iface, &addr);
    }

    // Assign Gateway
    if (net_addr_pton(AF_INET, CONFIG_LPS_GW, &addr) == 0) {
        net_if_ipv4_set_gw(iface, &addr);
    }

    printk("Static IPv4 configuration applied: %s\n", CONFIG_LPS_IP_ADDR);
}

static void initiate_wifi_connection(void)
{
    struct net_if *iface = net_if_get_default();
    struct wifi_connect_req_params wifi_params = {0};

    // Pull the credentials directly from your prj.conf
    wifi_params.ssid = CONFIG_WIFI_CREDENTIALS_STATIC_SSID;
    wifi_params.ssid_length = strlen(CONFIG_WIFI_CREDENTIALS_STATIC_SSID);
    wifi_params.psk = CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD;
    wifi_params.psk_length = strlen(CONFIG_WIFI_CREDENTIALS_STATIC_PASSWORD);
    
    wifi_params.security = WIFI_SECURITY_TYPE_PSK; // Assumes standard WPA2 Personal
    wifi_params.band = WIFI_FREQ_BAND_2_4_GHZ;

    if (rtc_has_cached_ap) {
        printk("Using cached BSSID (%x:%x:%x:%x:%x:%x) and Channel (%d). Skipping scan.\n",
	       rtc_cached_bssid[0], rtc_cached_bssid[1], rtc_cached_bssid[2],
	       rtc_cached_bssid[3], rtc_cached_bssid[4], rtc_cached_bssid[5],
	       rtc_cached_channel);
	memcpy(wifi_params.bssid, rtc_cached_bssid, WIFI_MAC_ADDR_LEN);
        wifi_params.channel = rtc_cached_channel;
    } else {
        printk("No cache found. Performing full network scan.\n");
        memset(wifi_params.bssid, 0, WIFI_MAC_ADDR_LEN);
        wifi_params.channel = WIFI_CHANNEL_ANY;
    }

    
    printk("Waking up Wi-Fi radio and requesting connection...\n");
    if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &wifi_params,
		 sizeof(struct wifi_connect_req_params))) {
        printk("Failed to push Wi-Fi connect request.\n");
    }
}


static void wifi_mgmt_event_handler(struct net_mgmt_event_callback *cb,
                                    uint64_t mgmt_event, struct net_if *iface)
{
    if (mgmt_event == NET_EVENT_WIFI_CONNECT_RESULT) {
        const struct wifi_status *status = (const struct wifi_status *)cb->info;

        if (status->status == 0) {
            printk("Wi-Fi associated with Access Point successfully!\n");

	    // --- Cache the BSSID and Channel ---
            struct wifi_iface_status iface_status = {0};
            if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, net_if_get_default(), 
                         &iface_status, sizeof(struct wifi_iface_status)) == 0) {
                
                memcpy(rtc_cached_bssid, iface_status.bssid, WIFI_MAC_ADDR_LEN);
                rtc_cached_channel = iface_status.channel;
                rtc_has_cached_ap = true;
                printk("Saved BSSID and Channel %d to RTC memory.\n", rtc_cached_channel);
            }
	    
	    apply_static_ip_config();

	    wifi_connection_is_alive=true;
            k_sem_give(&wifi_connected);
	    
        } else {
            printk("Wi-Fi connection failed with status: %d\n", status->status);
	    rtc_has_cached_ap = false;
        }
    }
}

void lps_wifi_prepare_connection(void)
{
    if (!wifi_connection_is_alive) {
	// Start listening for the Wi-Fi connection result
	net_mgmt_init_event_callback(&wifi_cb, wifi_mgmt_event_handler, 
				     NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);

	// Tell the radio to turn on and connect using your Kconfig credentials
	initiate_wifi_connection();
    
	// Wait up to 10 seconds for the Wi-Fi radio to associate
	printk("Waiting for Wi-Fi...\n");
	if (k_sem_take(&wifi_connected, K_SECONDS(20)) != 0) {
	    printk("Wi-Fi timeout. Going back to sleep.\n");
	}
    }
}

void lps_wifi_teardown_connection(void)
{
    // Send the 802.11 Deauth frame to the AP
    if (net_mgmt(NET_REQUEST_WIFI_DISCONNECT,
		 net_if_get_default(), NULL, 0) == 0) {
        printk("Sent Wi-Fi Deauth frame.\n");
    }
}
