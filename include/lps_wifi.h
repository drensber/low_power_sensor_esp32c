#ifndef LPS_WIFI_H
#define LPS_WIFI_H

/**
 * @brief Wakes the Wi-Fi radio and establishes a connection to the AP.
 * Applies either a cached "Fake Static" DHCP lease or initiates 
 * a full DHCP DORA sequence if the cache is empty/invalid.
 */
void lps_wifi_prepare_connection(void);

/**
 * @brief Gracefully tears down the Wi-Fi connection.
 * Sends an 802.11 Deauth frame to the AP to clear the router's 
 * state table and prevent "Ghost Client" association rejections.
 */
void lps_wifi_teardown_connection(void);

/**
 * @brief Invalidates the RTC DHCP cache.
 * Used as a reactive fallback when network routing fails, forcing 
 * a full, clean DHCP request on the next boot cycle.
 */
void lps_wifi_invalidate_dhcp_cache(void);

#endif /* LPS_WIFI_H */
