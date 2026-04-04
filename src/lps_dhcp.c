#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_pkt.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>
#include "lps_dhcp.h"

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_MAGIC_COOKIE 0x63825363
#define DHCP_TIMEOUT_MS 6000

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

#define OPT_SUBNET_MASK 1
#define OPT_ROUTER      3
#define OPT_REQ_IP      50
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_PARAM_REQ   55
#define OPT_END         255

struct __attribute__((packed)) eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
};

struct __attribute__((packed)) ip_hdr {
    uint8_t  vhl;
    uint8_t  tos;
    uint16_t len;
    uint16_t id;
    uint16_t off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
};

struct __attribute__((packed)) udp_hdr {
    uint16_t src;
    uint16_t dst;
    uint16_t len;
    uint16_t csum;
};

struct __attribute__((packed)) dhcp_msg {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic_cookie;
    uint8_t  options[64]; 
};

struct __attribute__((packed)) raw_dhcp_packet {
    struct eth_hdr  eth;
    struct ip_hdr   ip;
    struct udp_hdr  udp;
    struct dhcp_msg dhcp;
};

static uint16_t calc_ip_csum(const uint8_t *buf, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i += 2) {
	sum += (buf[i] << 8) | buf[i + 1];
    }
    while (sum >> 16) {
	sum = (sum & 0xffff) + (sum >> 16);
    }
    return htons((uint16_t)~sum);
}

static int push_raw_frame(struct net_if *iface, struct raw_dhcp_packet *tx_pkt) {
    struct net_pkt *pkt =
	net_pkt_alloc_with_buffer(iface, sizeof(*tx_pkt),
				  AF_UNSPEC, 0, K_NO_WAIT);
    if (!pkt) {
	return -1;
    }

    if (net_pkt_write(pkt, tx_pkt, sizeof(*tx_pkt)) < 0) {
        net_pkt_unref(pkt);
        return -1;
    }
    net_pkt_cursor_init(pkt);

    const struct device *dev = net_if_get_device(iface);
    const struct ethernet_api *api = (const struct ethernet_api *)dev->api;

    int ret = api->send(dev, pkt);
    
    // We ONLY unref it if the hardware rejected the handoff entirely.
    if (ret < 0) {
        net_pkt_unref(pkt); 
    }
    return ret;
}

int lps_dhcp_run(struct in_addr *ip,
		 struct in_addr *netmask,
		 struct in_addr *gw)
{
    int sock = -1;
    int ret = -1; 
    struct net_if *iface = net_if_get_default();
    struct net_linkaddr *mac = net_if_get_link_addr(iface);
    uint32_t server_id = 0;
    uint32_t transaction_id = sys_rand32_get() ^ k_cycle_get_32();
    
    struct raw_dhcp_packet tx_pkt;
    struct dhcp_msg rx_dhcp; 

    if (mac->len != 6) return -1;

    // Setup recieve hook
    struct in_addr dummy_ip, dummy_mask;
    net_addr_pton(AF_INET, "0.0.0.0", &dummy_mask); 
    
    if (ip->s_addr != 0) {
        dummy_ip.s_addr = ip->s_addr;
        printk("Listening for Unicast replies on expired IP...\n");
    } else {
        net_addr_pton(AF_INET, "169.254.1.1", &dummy_ip);
        printk("Listening for Broadcast replies on dummy IP...\n");
    }
    
    net_if_ipv4_addr_add(iface, &dummy_ip, NET_ADDR_MANUAL, 0);
    net_if_ipv4_set_netmask_by_addr(iface, &dummy_ip, &dummy_mask);

    printk("Opening UDP socket...\n");
    sock = zsock_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        printk("DHCP ERROR: zsock_socket failed. errno: %d\n", errno);
        goto cleanup;
    }

    struct sockaddr_in bind_addr = {0};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(DHCP_CLIENT_PORT);
    bind_addr.sin_addr.s_addr = INADDR_ANY; 
    
    printk("Binding socket to port 68...\n");
    if (zsock_bind(sock,
		   (struct sockaddr *)&bind_addr,
		   sizeof(bind_addr)) < 0) {
        printk("DHCP ERROR: zsock_bind failed. errno: %d\n", errno);
        goto cleanup;
    }

    // Set up the poll file descriptor structure
    struct zsock_pollfd poll_fd = {
        .fd = sock,
        .events = ZSOCK_POLLIN,
    };

    // Assemble the monolithic frame
    memset(&tx_pkt, 0, sizeof(tx_pkt));
    memset(tx_pkt.eth.dst, 0xFF, 6);
    memcpy(tx_pkt.eth.src, mac->addr, 6);
    tx_pkt.eth.type = htons(0x0800); 

    tx_pkt.ip.vhl = 0x45; 
    tx_pkt.ip.tos = 0;
    tx_pkt.ip.len = htons(sizeof(struct ip_hdr) +
			  sizeof(struct udp_hdr) +
			  sizeof(struct dhcp_msg));
    tx_pkt.ip.id = htons(1);
    tx_pkt.ip.ttl = 64;
    tx_pkt.ip.proto = 17; 
    tx_pkt.ip.src = 0;    
    tx_pkt.ip.dst = 0xFFFFFFFF; 
    tx_pkt.ip.csum = calc_ip_csum((uint8_t *)&tx_pkt.ip,
				  sizeof(struct ip_hdr));

    tx_pkt.udp.src = htons(DHCP_CLIENT_PORT);
    tx_pkt.udp.dst = htons(DHCP_SERVER_PORT);
    tx_pkt.udp.len = htons(sizeof(struct udp_hdr) +
			   sizeof(struct dhcp_msg));

    tx_pkt.dhcp.op = 1; 
    tx_pkt.dhcp.htype = 1; 
    tx_pkt.dhcp.hlen = 6;
    tx_pkt.dhcp.xid = transaction_id;
    tx_pkt.dhcp.flags = htons(0x8000); 
    memcpy(tx_pkt.dhcp.chaddr, mac->addr, 6);
    tx_pkt.dhcp.magic_cookie = htonl(DHCP_MAGIC_COOKIE);

    uint8_t *opt = tx_pkt.dhcp.options;
    *opt++ = OPT_MSG_TYPE; *opt++ = 1;
    *opt++ = DHCP_DISCOVER;
    *opt++ = OPT_PARAM_REQ; *opt++ = 2;
    *opt++ = OPT_SUBNET_MASK;
    *opt++ = OPT_ROUTER; 
    *opt++ = OPT_END;

    printk("Executing Direct Hardware Handoff (DISCOVER)...\n");
    if (push_raw_frame(iface, &tx_pkt) < 0) {
        printk("DHCP ERROR: push_raw_frame failed.\n");
        goto cleanup;
    }

    // Wait for offer
    bool offer_received = false;
    printk("Waiting up to %dms for DHCP OFFER...\n", DHCP_TIMEOUT_MS);
    while (!offer_received) {
	 // Wait strictly for DHCP_TIMEOUT_MS ms
        int poll_res = zsock_poll(&poll_fd, 1, DHCP_TIMEOUT_MS);
        
        if (poll_res == 0) {
            printk("DHCP WARNING: Poll timeout waiting for OFFER.\n");
            goto cleanup;
        } else if (poll_res < 0) {
            printk("DHCP ERROR: zsock_poll failed. errno: %d\n", errno);
            goto cleanup;
        }

        // We use ZSOCK_MSG_DONTWAIT to guarantee recvfrom never blocks
        if (zsock_recvfrom(sock, &rx_dhcp, sizeof(rx_dhcp),
			   ZSOCK_MSG_DONTWAIT, NULL, NULL) < 0) {
            continue; // Could be a spurious wake, just loop back to poll
        }

        if (rx_dhcp.op == 2 && rx_dhcp.xid == transaction_id) {
            opt = rx_dhcp.options;
            while (*opt != OPT_END && opt <
		   rx_dhcp.options + sizeof(rx_dhcp.options)) {
                if (*opt == 0) {
		    opt++;
		    continue;
		} 
                uint8_t tag = *opt++; uint8_t len = *opt++;
                if (tag == OPT_MSG_TYPE && *opt == DHCP_OFFER) {
		    offer_received = true;
		}
                if (tag == OPT_SERVER_ID) {
		    memcpy(&server_id, opt, 4);
		}
                opt += len;
            }
        }
    }

    // Send DHCP request
    memset(tx_pkt.dhcp.options, 0, sizeof(tx_pkt.dhcp.options));
    opt = tx_pkt.dhcp.options;
    *opt++ = OPT_MSG_TYPE;
    *opt++ = 1;
    *opt++ = DHCP_REQUEST;
    *opt++ = OPT_REQ_IP;
    *opt++ = 4;
    memcpy(opt, &rx_dhcp.yiaddr, 4);
    opt += 4;
    *opt++ = OPT_SERVER_ID;*opt++ = 4;
    memcpy(opt, &server_id, 4);
    opt += 4;
    *opt++ = OPT_END;

    tx_pkt.ip.csum = 0;
    tx_pkt.ip.csum = calc_ip_csum((uint8_t *)&tx_pkt.ip,
				  sizeof(struct ip_hdr));

    printk("Executing Direct Hardware Handoff (REQUEST)...\n");
    if (push_raw_frame(iface, &tx_pkt) < 0) {
        printk("DHCP ERROR: push_raw_frame (REQUEST) failed.\n");
        goto cleanup;
    }

    // Wait for ACK
    printk("Waiting for DHCP ACK...\n");
    while (1) {
        int poll_res = zsock_poll(&poll_fd, 1, 3000); 
        
        if (poll_res <= 0) {
            printk("DHCP WARNING: Poll timeout/error waiting for ACK.\n");
            goto cleanup;
        }

        if (zsock_recvfrom(sock, &rx_dhcp, sizeof(rx_dhcp),
			   ZSOCK_MSG_DONTWAIT, NULL, NULL) < 0) {
            continue;
        }

        if (rx_dhcp.op == 2 && rx_dhcp.xid == transaction_id) {
            ip->s_addr = rx_dhcp.yiaddr;
            
            opt = rx_dhcp.options;
            while (*opt != OPT_END && opt <
		   rx_dhcp.options + sizeof(rx_dhcp.options)) {
                if (*opt == 0) { opt++; continue; }
                uint8_t tag = *opt++;
		uint8_t len = *opt++;
                if (tag == OPT_MSG_TYPE && *opt == DHCP_ACK) {
		    ret = 0;
		}
                if (tag == OPT_SUBNET_MASK) {
		    memcpy(&netmask->s_addr, opt, 4);
		}
                if (tag == OPT_ROUTER) {
		    memcpy(&gw->s_addr, opt, 4);
		}
                opt += len;
            }
            if (ret == 0) {
		break;
	    }
        }
    }

cleanup:
    if (sock >= 0) {
	zsock_close(sock);
    }
    net_if_ipv4_addr_rm(iface, &dummy_ip);
    return ret;
}

