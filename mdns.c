/*
 * mdns.c  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Respondant mDNS basé sur lwIP (pico_lwip_mdns). Annonce le service
 * _withrottle._tcp.local. → Engine Driver découvre le gateway sans
 * saisie manuelle d'IP/port.
 *
 * Séquence obligatoire (SDK 2.2.0) : mdns_resp_init() puis
 * mdns_resp_add_netif() avant mdns_resp_add_service() — sinon hardfault
 * dans mdns_build_service_domain().
 */

#include <string.h>

#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"

#include "config.h"
#include "mdns.h"

static const char *TAG = "mdns";

// Hostname (A record) and WiThrottle service instance
#define MDNS_HOSTNAME    "wibidib"
#define MDNS_INSTANCE    "WiBiDiB"
#define MDNS_SERVICE     "_withrottle"
#define MDNS_PROTO       DNSSD_PROTO_TCP

bool mdns_init(struct netif *ap_netif) {
    if (!ap_netif) {
        LOG_ERROR(TAG, "mdns_init: netif NULL");
        return false;
    } else {
        LOG_INFO(TAG, "mdns_init: OK ");
    }

    cyw43_arch_lwip_begin();

    mdns_resp_init();

    if (!mdns_resp_netif_active(ap_netif)) {
        err_t r = mdns_resp_add_netif(ap_netif, MDNS_HOSTNAME);
        if (r != ERR_OK) {
            LOG_ERROR(TAG, "mdns_resp_add_netif failed: %d", r);
            cyw43_arch_lwip_end();
            return false;
        }
    }

    s8_t slot = mdns_resp_add_service(ap_netif, MDNS_INSTANCE,
                                       MDNS_SERVICE, MDNS_PROTO,
                                       WITHROTTLE_PORT, NULL, NULL);
    LOG_INFO(TAG, "slot %d instance %s service %s proto %s port %s",
             slot, MDNS_INSTANCE, MDNS_SERVICE, MDNS_PROTO,
             WITHROTTLE_PORT);
    if (slot < 0) {
        LOG_ERROR(TAG, "mdns_resp_add_service failed: %d", slot);
        cyw43_arch_lwip_end();
        return false;
    }

    mdns_resp_announce(ap_netif);

    cyw43_arch_lwip_end();

    LOG_INFO(TAG, "mDNS up: %s._withrottle._tcp.local. port %d on %s",
             MDNS_INSTANCE, WITHROTTLE_PORT, ip4addr_ntoa(netif_ip4_addr(ap_netif)));
    return true;
}

