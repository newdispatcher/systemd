/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright (C) 2014 Intel Corporation. All rights reserved.

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <netinet/ether.h>
#include <linux/if.h>

#include "networkd-link.h"
#include "network-internal.h"

#include "sd-icmp6-nd.h"
#include "sd-dhcp6-client.h"

static int dhcp6_lease_information_acquired(sd_dhcp6_client *client,
                                        Link *link) {
        return 0;
}

static int dhcp6_address_handler(sd_rtnl *rtnl, sd_rtnl_message *m,
                                 void *userdata) {
        _cleanup_link_unref_ Link *link = userdata;
        int r;

        assert(link);

        r = sd_rtnl_message_get_errno(m);
        if (r < 0 && r != -EEXIST) {
                log_link_error(link, "Could not set DHCPv6 address: %s",
                               strerror(-r));

                link_enter_failed(link);

        } else if (r >= 0)
                link_rtnl_process_address(rtnl, m, link->manager);

        return 1;
}

static int dhcp6_address_update(Link *link, struct in6_addr *ip6_addr,
                                uint8_t prefixlen, uint32_t lifetime_preferred,
                                uint32_t lifetime_valid) {
        int r;
        _cleanup_address_free_ Address *addr = NULL;

        r = address_new_dynamic(&addr);
        if (r < 0)
                return r;

        addr->family = AF_INET6;
        memcpy(&addr->in_addr.in6, ip6_addr, sizeof(*ip6_addr));

        addr->flags = IFA_F_NOPREFIXROUTE;
        addr->prefixlen = 64;

        addr->cinfo.ifa_prefered = lifetime_preferred;
        addr->cinfo.ifa_valid = lifetime_valid;

        log_link_struct(link, LOG_INFO, "MESSAGE=%-*s: DHCPv6 address "SD_ICMP6_ADDRESS_FORMAT_STR"/%d timeout preferred %d valid %d",
                        IFNAMSIZ,
                        link->ifname, SD_ICMP6_ADDRESS_FORMAT_VAL(addr->in_addr.in6),
                        addr->prefixlen, lifetime_preferred, lifetime_valid,
                        NULL);

        r = address_update(addr, link, dhcp6_address_handler);
        if (r < 0)
                log_link_warning(link, "Could not assign DHCPv6 address: %s",
                                strerror(-r));

        return r;
}

static int dhcp6_lease_address_acquired(sd_dhcp6_client *client, Link *link) {
        int r;
        sd_dhcp6_lease *lease;
        struct in6_addr ip6_addr;
        uint32_t lifetime_preferred, lifetime_valid;
        uint8_t prefixlen;

        r = sd_dhcp6_client_get_lease(client, &lease);
        if (r < 0)
                return r;

        sd_dhcp6_lease_reset_address_iter(lease);

        while (sd_dhcp6_lease_get_address(lease, &ip6_addr,
                                                &lifetime_preferred,
                                                &lifetime_valid) >= 0) {

                r = sd_icmp6_ra_get_prefixlen(link->icmp6_router_discovery,
                                        &ip6_addr, &prefixlen);
                if (r < 0 && r != -EADDRNOTAVAIL) {
                        log_link_warning(link, "Could not get prefix information: %s",
                                        strerror(-r));
                        return r;
                }

                if (r == -EADDRNOTAVAIL)
                        prefixlen = 128;

                r = dhcp6_address_update(link, &ip6_addr, prefixlen,
                                        lifetime_preferred, lifetime_valid);
                if (r < 0)
                        return r;
        }

        return 0;
}

static void dhcp6_handler(sd_dhcp6_client *client, int event, void *userdata) {
        int r;
        Link *link = userdata;

        assert(link);
        assert(link->network);
        assert(link->manager);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return;

        switch(event) {
        case DHCP6_EVENT_STOP:
        case DHCP6_EVENT_RESEND_EXPIRE:
        case DHCP6_EVENT_RETRANS_MAX:
                log_link_debug(link, "DHCPv6 event %d", event);
                break;

        case DHCP6_EVENT_IP_ACQUIRE:
                r = dhcp6_lease_address_acquired(client, link);
                if (r < 0) {
                        link_enter_failed(link);
                        return;
                }

                /* fall through */
        case DHCP6_EVENT_INFORMATION_REQUEST:
                r = dhcp6_lease_information_acquired(client, link);
                if (r < 0) {
                        link_enter_failed(link);
                        return;
                }

                break;

        default:
                if (event < 0)
                        log_link_warning(link, "DHCPv6 error: %s",
                                         strerror(-event));
                else
                        log_link_warning(link, "DHCPv6 unknown event: %d",
                                         event);
                return;
        }
}

static int dhcp6_configure(Link *link, int event) {
        int r;
        bool information_request;

        assert_return(link, -EINVAL);

        if (link->dhcp6_client) {
                if (event != ICMP6_EVENT_ROUTER_ADVERTISMENT_MANAGED)
                        return 0;

                r = sd_dhcp6_client_get_information_request(link->dhcp6_client,
                                                        &information_request);
                if (r < 0) {
                        log_link_warning(link, "Could not get DHCPv6 Information request setting: %s",
                                        strerror(-r));
                        link->dhcp6_client =
                                sd_dhcp6_client_unref(link->dhcp6_client);
                        return r;
                }

                if (!information_request)
                        return r;

                r = sd_dhcp6_client_set_information_request(link->dhcp6_client,
                                                        false);
                if (r < 0) {
                        log_link_warning(link, "Could not unset DHCPv6 Information request: %s",
                                        strerror(-r));
                        link->dhcp6_client =
                                sd_dhcp6_client_unref(link->dhcp6_client);
                        return r;
                }

                r = sd_dhcp6_client_start(link->dhcp6_client);
                if (r < 0) {
                        log_link_warning(link, "Could not restart DHCPv6 after enabling Information request: %s",
                                        strerror(-r));
                        link->dhcp6_client =
                                sd_dhcp6_client_unref(link->dhcp6_client);
                        return r;
                }

                return r;
        }

        r = sd_dhcp6_client_new(&link->dhcp6_client);
        if (r < 0)
                return r;

        r = sd_dhcp6_client_attach_event(link->dhcp6_client, NULL, 0);
        if (r < 0) {
                link->dhcp6_client = sd_dhcp6_client_unref(link->dhcp6_client);
                return r;
        }

        r = sd_dhcp6_client_set_mac(link->dhcp6_client,
                                    (const uint8_t *) &link->mac,
                                    sizeof (link->mac), ARPHRD_ETHER);
        if (r < 0) {
                link->dhcp6_client = sd_dhcp6_client_unref(link->dhcp6_client);
                return r;
        }

        r = sd_dhcp6_client_set_index(link->dhcp6_client, link->ifindex);
        if (r < 0) {
                link->dhcp6_client = sd_dhcp6_client_unref(link->dhcp6_client);
                return r;
        }

        r = sd_dhcp6_client_set_callback(link->dhcp6_client, dhcp6_handler,
                                         link);
        if (r < 0) {
                link->dhcp6_client = sd_dhcp6_client_unref(link->dhcp6_client);
                return r;
        }

        if (event == ICMP6_EVENT_ROUTER_ADVERTISMENT_OTHER) {
                r = sd_dhcp6_client_set_information_request(link->dhcp6_client,
                                                        true);
                if (r < 0) {
                        link->dhcp6_client =
                                sd_dhcp6_client_unref(link->dhcp6_client);
                        return r;
                }
        }

        r = sd_dhcp6_client_start(link->dhcp6_client);
        if (r < 0)
                link->dhcp6_client = sd_dhcp6_client_unref(link->dhcp6_client);

        return r;
}

static void icmp6_router_handler(sd_icmp6_nd *nd, int event, void *userdata) {
        Link *link = userdata;

        assert(link);
        assert(link->network);
        assert(link->manager);

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return;

        switch(event) {
        case ICMP6_EVENT_ROUTER_ADVERTISMENT_NONE:
        case ICMP6_EVENT_ROUTER_ADVERTISMENT_PREFIX_EXPIRED:
                return;

        case ICMP6_EVENT_ROUTER_ADVERTISMENT_TIMEOUT:
        case ICMP6_EVENT_ROUTER_ADVERTISMENT_OTHER:
        case ICMP6_EVENT_ROUTER_ADVERTISMENT_MANAGED:
                dhcp6_configure(link, event);

                break;

        default:
                if (event < 0)
                        log_link_warning(link, "ICMPv6 error: %s",
                                         strerror(-event));
                else
                        log_link_warning(link, "ICMPv6 unknown event: %d",
                                         event);

                break;
        }

}

int icmp6_configure(Link *link) {
        int r;

        assert_return(link, -EINVAL);

        r = sd_icmp6_nd_new(&link->icmp6_router_discovery);
        if (r < 0)
                return r;

        r = sd_icmp6_nd_attach_event(link->icmp6_router_discovery, NULL, 0);
        if (r < 0)
                return r;

        r = sd_icmp6_nd_set_mac(link->icmp6_router_discovery, &link->mac);
        if (r < 0)
                return r;

        r = sd_icmp6_nd_set_index(link->icmp6_router_discovery, link->ifindex);
        if (r < 0)
                return r;

        r = sd_icmp6_nd_set_callback(link->icmp6_router_discovery,
                                icmp6_router_handler, link);

        return r;
}
