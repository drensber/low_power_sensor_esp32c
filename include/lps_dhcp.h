#ifndef LPS_MICRO_DHCP_H
#define LPS_MICRO_DHCP_H

/**
 * @brief Executes a synchronous, blocking DHCP DORA sequence
 *        over raw UDP sockets.
 * @param ip Parsed IP address assigned by the router.
 * @param netmask Parsed Subnet Mask.
 * @param gw Parsed Router/Gateway address.
 * @return 0 on success, negative error code on timeout or failure.
 */
int lps_dhcp_run(struct in_addr *ip,
		       struct in_addr *netmask,
		       struct in_addr *gw);

#endif /* LPS_MICRO_DHCP_H */
