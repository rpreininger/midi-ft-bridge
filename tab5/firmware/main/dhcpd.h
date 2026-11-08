// DHCP server with fixed leases for the hotspot (see dhcpd.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "net.h"

typedef void (*dhcpd_cb_t)(const char *ip, bool fixed);

bool dhcpd_parse_mac(const char *s, uint8_t mac[6]);
void dhcpd_start(const char *server_ip, const net_lease_t *leases, int n, dhcpd_cb_t cb);
