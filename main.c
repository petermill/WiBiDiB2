/*
 * main.c — BIDIBrc Pico 2W
 * Point d'entrée principal
 *
 * v0.2 : ajout WiFi AP + TCP WiThrottle
 * Le bus BiDiB (PIO) et le WiFi tournent en parallèle dans la même boucle :
 *   - cyw43_arch_poll() gère les callbacks TCP/lwIP
 *   - Les ISR PIO (bidib_pio_rx_isr / tx_isr) sont déclenchées par hardware
 *     indépendamment de la boucle → pas d'interférence
 */

#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/pio.h"

#include "bidib.h"
#include "tcp_server.h"
#include "smartphone_if.h"
#include "bidib_client_parser.h"
#include "config.h"
#include "mdns.h"
#include "status_led.h" 
#include "roster.h"
#include "roster_store.h"

extern volatile bool g_bidib_logon_allowed;
static const char *TAG = "main";

int main(void)
{
    stdio_init_all();
   // sleep_ms(3000);  // attendre USB serial
    while (!stdio_usb_connected()) {
    sleep_ms(100);
         }
printf("USB connected, starting init...\n");
    LOG_INFO(TAG,"=== WI_BIDIB_ED Pico 2W v0.2 ===");
    stdio_flush();
printf("[roster] sizeof(roster_t)=%u, MAX_ENTRIES=%u\n",
       (unsigned)sizeof(roster_t), (unsigned)ROSTER_MAX_ENTRIES);
    // ── BiDiB PIO  ──────────────────────────────────────────────────
    // ISR RX/TX enregistrées dans bidib_init(), tournent en hardware
    bidib_init();
    LOG_INFO(TAG,"BiDiB PIO OK");
    init_bidib_client();
    LOG_INFO(TAG,"BiDiB client init OK");

    // ── WiFi AP + TCP WiThrottle ──────────────────────────────────────────────
    smartphone_if_init();   // init table throttle[] + UID BiDiB

        struct netif *ap_netif = wifi_init_softap();
        if (!ap_netif) {
            printf("WiFi AP ERREUR — on continue sans WiFi\n");
            } else {
            if (!tcp_server_init()) {
                printf("TCP server ERREUR\n");
            } else {
                LOG_INFO(TAG,"WiFi AP + TCP OK — SSID:myssid port:5550");
            }
            // mDNS : advertise the WiThrottle service → Engine Driver discovery
              mdns_init(ap_netif);
            }

            roster_store_init_load();

    printf("Boucle principale\n");


    bidib_flush_rx();                       /* jeter ce que l'ISR a accumule pendant l'init */
   g_bidib_connect = BIDIB_DISCONNECTED;   /* etat propre */
   g_bidib_logon_allowed = true;           /* le premier jeton LOGON declenche maintenant le logon */
    printf("logon autorise\n");
   
   // ── Boucle principale ─────────────────────────────────────────────────────
    //
    // cyw43_arch_poll() déclenche les callbacks TCP (recv, accept, err)
    //   → process_rx_withrottle() appelé à l'intérieur
    //
    // Les ISR BiDiB PIO tournent indépendamment (hardware IRQ)
    //   → bidib_pio_rx_isr() / bidib_pio_tx_isr() non affectés par le poll
    //
    while (1) {
        cyw43_arch_poll();  // traite WiFi + lwIP callbacks
        run_bidib_client(); 
        status_led_set(bidib_is_ready());   // cheap : ne fait quelque chose que sur changement
       roster_store_poll();
    }

    cyw43_arch_deinit();
    return 0;
}