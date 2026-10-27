/*
 * withrottle_if.c  —  WiBiDiB  (Pico 2W)
 *
 * Passerelle Engine Driver (WiThrottle) → BiDiBus
 * Copyright (c) 2010 Pierre Moulin — GNU GPL v2
 *
 * Nettoyage v2 :
 *   - Suppression code WiThrottle (MT+/MTL) — Engine Driver uniquement (M0+/M0L)
 *   - Suppression isWT, MAX_LOCO_PER_THR=1 par slot (plus de doublon WT/ED)
 *   - Correction bug actionKey '*' dans locoAction()
 *   - Correction réponse qV/qR (utilisait ak au lieu de Loco_actionKey)
 *   - Suppression ancien locoAdd commenté
 *   - TODO PHASE 2 : remplacer printf <t> et <f> par appels BiDiB
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>

#include "pico/stdlib.h"
#include "config.h"
#include "datatypes.h"
#include "withrottle_if.h"
#include "smartphone_if.h"
#include "tcp_server.h"
#include "bidib_client_parser.h"
#include "roster.h"


static const char *TAG = "withrottle_if";

// ─── Structure loco ───────────────────────────────────────────────────────────
// 1 loco par slot (Engine Driver uniquement)
typedef struct {
    char    Loco_actionKey[16]; // ex: "S14" ou "L4550"
    int     LocoState[29];      // état F0..F28
    int     dccAdress;
    int     newSpeed;
    int     oldSpeed;
    uint8_t dir;                // 1=avant, 0=arrière
    uint8_t slot;
    bool    longAddr;           // true si actionKey 'L'
    char    th[3];              // id du throttle ED (ex: "0", "6")
} Engine_t;

static Engine_t Loco[MAX_THROTTLES];

static const int heartbeatTimeout = HEARTBEAT_TIMEOUT_S;
static uint8_t   heartbeatEnable[MAX_THROTTLES];
static uint32_t  heartbeat[MAX_THROTTLES];

// Variables de parsing
static char    LocoAdress_g[14];
static char    actionKey_g[48];   // texte apres <;> (action, ou id roster 'E...' jusqu'a 31 car.)
static char    locoKey_g[16];     // cle de loco avant <;> ex: "L4528" / "S8"
static int     Throttle_g = 0;

// ─── throttle_stop() ─────────────────────────────────────────────────────────
void throttle_stop(uint8_t slot) {
    printf("[withrottle_if] throttle_stop slot %d\n", slot);
    
    heartbeatEnable[slot] = false;
    heartbeat[slot]       = 0;
    Loco[slot].newSpeed   = 0;
}

// Recherche dans le roster : adresse + type (court/long).
static const roster_entry_t *find_entry(int locoAddr, bool isLong) {
    for (uint8_t i = 0; i < roster.count; i++) {
        const roster_entry_t *e = roster_get(i);
        if (e && e->dccAddress == locoAddr && (bool)e->longAddress == isLong)
            return e;
    }
    return NULL;
}

// Compatibilite avec l'ancien appel (adresse seule) : <=127 => courte
const roster_entry_t * findRosterEntry(int locoAddr) {
    return find_entry(locoAddr, locoAddr > 127);
}

// ─── locoAdd() ───────────────────────────────────────────────────────────────
// Engine Driver uniquement : M0+ / M0L / V0 / R1 / s1
static void locoAdd(const char *th, const char *ak, const char *ref, uint8_t slot) {
    char    msg[128];
    uint8_t length_msg = 0;
    struct tcp_pcb *pcb = throttle[slot].pcb;

    const roster_entry_t *e = find_entry(Loco[slot].dccAdress, Loco[slot].longAddr);
    LOG_INFO(TAG, "locoAdd dcc=%d %c -> roster %s", Loco[slot].dccAdress,
             Loco[slot].longAddr ? 'L' : 'S', e ? e->id : "(absent)");

    // 1 seule loco par slot : liberer la precedente cote ED avant d'en prendre une autre
    if (Loco[slot].Loco_actionKey[0] != '\0' && strcmp(Loco[slot].Loco_actionKey, ak) != 0) {
        int n = snprintf(msg, sizeof(msg), "M%s-%s<;>\n\n", th, Loco[slot].Loco_actionKey);
        send_msg(pcb, n, msg);
        LOG_INFO(TAG, "release previous %s", Loco[slot].Loco_actionKey);
    }
    memset(msg, 0, sizeof(msg));
    memcpy(Loco[slot].Loco_actionKey, ak, strlen(ak) + 1);

    // M<th>+<cle><;><ref>\n\n  -- ED attend sa propre cle (ex L4528) puis l'id roster (ex E4528)
    strncpy(Loco[slot].th, th, sizeof(Loco[slot].th) - 1);
    length_msg = (uint8_t)snprintf(msg, sizeof(msg), "M%s+%s<;>%s\n\n", th, ak, ref);
    send_msg(pcb, length_msg, msg);

    // M<th>L<cle><;>]\[F0]\[F1]\[...]\[F28\n\n  (29 libelles ; vide = fonction masquee)
    {
        static char lbl[29 * (ROSTER_STRING_MAX + 3) + 64];
        size_t n = 0;
        n += snprintf(lbl + n, sizeof(lbl) - n, "M%sL%s<;>", th, ak);
        for (int f = 0; f < 29; f++) {
            const char *lab = "";
            if (e) {
                if (f <= ROSTER_FUNC_MAX && e->functions[f].visible)
                    lab = e->functions[f].label;
            } else if (f == 0) {
                lab = "Feux";            // pas de roster : comportement d'origine
            }
            n += snprintf(lbl + n, sizeof(lbl) - n, "]\\[%s", lab);
            if (n >= sizeof(lbl) - 4) break;
        }
        n += snprintf(lbl + n, sizeof(lbl) - n, "\n\n");
        send_msg(pcb, (int)n, lbl);
    }

    // RAZ état locos en mémoire
    for (int fk = 0; fk < 29; fk++) Loco[slot].LocoState[fk] = 0;
    Loco[slot].newSpeed = 0;
    Loco[slot].oldSpeed = 0;
    Loco[slot].dir      = 1;

    // M0A<ak><;>V0\n\n
    length_msg = 0;
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; 
    length_msg++;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='A';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='V'; msg[length_msg++]='0';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    // M0A<ak><;>R1\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; length_msg=1;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='A';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='R'; msg[length_msg++]='1';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    // M0A<ak><;>s1\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; length_msg=1;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='A';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='s'; msg[length_msg++]='1';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);

    tcp_output(pcb);
    LOG_INFO(TAG," loco_wi_+  th=%s ak=%s dcc=%d slot=%d", th, ak,Loco[slot].dccAdress, slot );
}

// ─── locoRelease() ───────────────────────────────────────────────────────────
static void locoRelease(const char *th, const char *ak, uint8_t slot) {
    char    msg[64];
    uint8_t length_msg = 0;
    struct tcp_pcb *pcb = throttle[slot].pcb;

    heartbeat[slot]     = 0;
    Loco[slot].newSpeed = 0;

    // LOG_INFO(TAG,"locoRelease th=%s ak=%s dccAdress=%d", th, ak, Loco[slot].dccAdress );

    if (Loco[slot].dccAdress == 0) {
        LOG_INFO(TAG, "No loco address assigned for slot 0");
        return;
        }

    // M0-<ak><;>\n\n
    memset(msg, 0, sizeof(msg));
    msg[0]='M'; length_msg=1;
    memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
    msg[length_msg++]='-';
    memcpy(msg+length_msg, ak, strlen(ak)); length_msg += strlen(ak);
    msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
    msg[length_msg++]='\n'; msg[length_msg++]='\n';
    send_msg(pcb, length_msg, msg);
    tcp_output(pcb);
    Loco[slot].Loco_actionKey[0] = '\0';   // libere : plus de 'release previous' au prochain M+
}

// ─── locoAction() ────────────────────────────────────────────────────────────
static void locoAction(const char *th, char *ak, uint8_t slot) {
    char    msg[128];
    uint8_t length_msg = 0;
    struct tcp_pcb *pcb = throttle[slot].pcb;
    char    tmp[20]     = {};
    char    locoAddress[10] = {};
    
 //   LOG_INFO(TAG,"locoAction th=%s ak=%s", th, ak );
    
    if (Loco[slot].dccAdress == 0) {
        LOG_INFO(TAG, "No loco address assigned for slot 0");
        return;
        }

    // '*' : remplacer par l'actionKey réelle sauvegardée
    if (ak[0] == '*') {
        memcpy(ak, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey) + 1);
    }  

    // ── F : fonction ──────────────────────────────────────────────────────────
    // steve Todd
    //If momentary, the server should set the function state ON 
    // when it receives the DOWN, and OFF when it receives the UP.
    //If latching, the server TOGGLEs the state when it receives the DOWN. 
    // It ignores the UP.
    
if (ak[0] == 'F') {
        // Format : F<0|1><num>  ex: F10 = F0 on, F110 = F10 on
        int fstate = ak[1] - '0';                       // 1=DOWN, 0=UP
        int fn     = (int)strtol(ak + 2, NULL, 10);
        if (fn < 0 || fn > 28) return;

        const roster_entry_t *entry = find_entry(Loco[slot].dccAdress, Loco[slot].longAddr);
        bool known    = entry && fn <= ROSTER_FUNC_MAX && entry->functions[fn].label[0] != '\0';
        bool lockable = known ? entry->functions[fn].lockable : (fn != 2); // sans roster : tout sauf F2

        if (lockable) {
            if (fstate == 0) {                          // UP ignore pour une fonction a bascule
                return;
            }
            Loco[slot].LocoState[fn] = Loco[slot].LocoState[fn] ? 0 : 1;   // DOWN => toggle
        } else {
            Loco[slot].LocoState[fn] = fstate;          // momentanee : suit DOWN/UP
        }
        fstate = Loco[slot].LocoState[fn];
        LOG_INFO(TAG, "F%d lockable=%d state=%d", fn, lockable, fstate);

       


/* previous version sans roster

     //   LOG_INFO(TAG, "previous fstate Loco[slot].LocoState[fn] %d",Loco[slot].LocoState[fn]);
        int fstate = ak[1] - '0';               // 0 ou 1              
     //   LOG_INFO(TAG, "query for fstate Loco[slot].LocoState[fn] %d",Loco[slot].LocoState[fn]); 
           if (fn != 2) {
                // we expect everything other than F2 is lockable
                if (fstate == 1) {
                    // It ignores the UP.
                    LOG_INFO(TAG, "Skip fstate = 1 because function is lockable: %d", fn);
                    return;
                }
                else {
                    // TOGGLEs the state when it receives the DOWN
                    Loco[slot].LocoState[fn] = Loco[slot].LocoState[fn] == 0 ? 1 : 0;
                    fstate = Loco[slot].LocoState[fn];
                    LOG_INFO(TAG, "Toggled current state fn = %d, state: %d", fn, fstate);
                }
            }
           else {
               Loco[slot].LocoState[fn] = fstate;
            }
       */
            
        // Renvoyer l'état confirmé au smartphone : M0A<ak><;>F<state><num>
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; length_msg=1;
        memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
        msg[length_msg++]='A';
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
        msg[length_msg++]='F';
        itoa(fstate, tmp, 10);
        memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        itoa(fn, tmp, 10);
        memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        msg[length_msg++]='\n'; msg[length_msg++]='\n';

        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);
              
        // Calculer active selon fn
            uint8_t active = 0;
            if (fn <= 4)  active |= BIDIB_CS_DRIVE_F0F4_BIT;
            else if (fn <= 8)  active |= BIDIB_CS_DRIVE_F5F8_BIT;
            else if (fn <= 12) active |= BIDIB_CS_DRIVE_F9F12_BIT;
            else if (fn <= 20) active |= BIDIB_CS_DRIVE_F13F20_BIT;
            else               active |= BIDIB_CS_DRIVE_F21F28_BIT;

            LOG_INFO(TAG,"dccAdress %d  LocoState %02x   Active %02x ",
                Loco[slot].dccAdress, Loco[slot].LocoState[fn], active);

            bidib_send_cs_drive(Loco[slot].dccAdress, Loco[slot].newSpeed,
                    Loco[slot].dir, &Loco[slot].LocoState[0], active);        
    }

    // ── qV / qR : demande de l'état courant ──────────────────────────────────
    else if (ak[0] == 'q') {
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; length_msg=1;
        memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
        msg[length_msg++]='A';
        // Répondre avec l'actionKey réelle (ex: S14), pas 'q'
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
        if (ak[1] == 'V') {
            msg[length_msg++]='V';
            itoa(Loco[slot].newSpeed, tmp, 10);
            memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        } else { // qR
            msg[length_msg++]='R';
            itoa(Loco[slot].dir, tmp, 10);
            memcpy(msg+length_msg, tmp, strlen(tmp)); length_msg += strlen(tmp);
        }
        msg[length_msg++]='\n'; msg[length_msg++]='\n';
        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);
    }

    // ── V : vitesse ───────────────────────────────────────────────────────────
    else if (ak[0] == 'V') {       
        int spd = (int)strtol(ak + 1, NULL, 10);
        Loco[slot].newSpeed = spd;
        uint8_t active = 1;

        gpio_put(BIDIB_PIN_TEST , 1);
        busy_wait_us_32(4);
        gpio_put(BIDIB_PIN_TEST , 0);

        LOG_INFO(TAG,"dccAdress %d  Speed %02x dir %02x  Active %02x ",
        Loco[slot].dccAdress, Loco[slot].newSpeed,Loco[slot].dir,active);
        LOG_INFO(TAG," call bidib_send_cs_drive() ");
        bidib_send_cs_drive(Loco[slot].dccAdress, spd,Loco[slot].dir,
            Loco[slot].LocoState,   // LocoState[29];      // état F0..F28
            active );
        // Renvoyer V confirmée au smartphone
    }

    // ── R : direction ─────────────────────────────────────────────────────────
    else if (ak[0] == 'R') {
        int dir = (int)strtol(ak + 1, NULL, 10);
        Loco[slot].dir      = dir;
        Loco[slot].newSpeed = 0;
        uint8_t active = 1;
             bidib_send_cs_drive(Loco[slot].dccAdress, Loco[slot].newSpeed,Loco[slot].dir,
            Loco[slot].LocoState,   // LocoState[29];      // état F0..F28
            active );
        // Renvoyer V0 au smartphone
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; length_msg=1;
        memcpy(msg+length_msg, th, strlen(th)); length_msg += strlen(th);
        msg[length_msg++]='A';
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        msg[length_msg++]='<'; msg[length_msg++]=';'; msg[length_msg++]='>';
        msg[length_msg++]='V'; msg[length_msg++]='0';
        msg[length_msg++]='\n'; msg[length_msg++]='\n';
        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);
    }

    // ── X : emergency stop ────RUN_STOP,     // DCC Running, all Engines Emergency Stop────────────────────────────
    else if (ak[0] == 'X') {
        // Xpressnet command 0x81 0x00 0x81 "Everything stopped (emergency stop)"
        //       set_command_station_status(STS_STOP);
    }

    // ── I : idle ──────────────────────────────────────────────────────────────
    else if (ak[0] == 'I') {
        uint8_t active = 1;
        Loco[slot].newSpeed = 0;
         bidib_send_cs_drive(Loco[slot].dccAdress, Loco[slot].newSpeed,Loco[slot].dir,
            Loco[slot].LocoState,   // LocoState[29];      // état F0..F28
            active );
    }

    // ── Q : quit loco ─────────────────────────────────────────────────────────
    else if (ak[0] == 'Q') {
        Loco[slot].newSpeed = 0;
        printf("dcc %d QUIT\n", Loco[slot].dccAdress);
    }
}

// ─── checkHeartbeat() ────────────────────────────────────────────────────────
static void checkHeartbeat(uint8_t slot) {
    char    msg[50];
    uint8_t length_msg;
    struct tcp_pcb *pcb = throttle[slot].pcb;
    char    tmp[20]     = {};
    uint32_t now        = now_ms();

    if (heartbeat[slot] > 0 &&
        (heartbeat[slot] + (uint32_t)heartbeatTimeout * 5000) < now) {

        Loco[slot].newSpeed = 0;
        heartbeat[slot]     = 0;

        // Envoyer V0 emergency
        memset(msg, 0, sizeof(msg));
        msg[0]='M'; length_msg=1;
        memcpy(msg+length_msg, Loco[slot].th, strlen(Loco[slot].th)); length_msg += strlen(Loco[slot].th);
        msg[length_msg++]='A';
        memcpy(msg+length_msg, Loco[slot].Loco_actionKey,
               strlen(Loco[slot].Loco_actionKey));
        length_msg += strlen(Loco[slot].Loco_actionKey);
        const char suffix[] = {'<',';','>','V','0','\n','\n'};
        memcpy(msg+length_msg, suffix, sizeof(suffix));
        length_msg += sizeof(suffix);
        send_msg(pcb, length_msg, msg);
        tcp_output(pcb);
    }
}


// ─── Envoi sur TCP avec controle (decoupe selon la place dispo) ─────────────
static bool tcp_write_all(struct tcp_pcb *pcb, const char *p, size_t len) {
    while (len) {
        u16_t room = tcp_sndbuf(pcb);
        if (room == 0) { tcp_output(pcb); LOG_INFO(TAG, "ERR tcp_sndbuf plein, %u octets perdus", (unsigned)len); return false; }
        u16_t n = (len < room) ? (u16_t)len : room;
        if (tcp_write(pcb, p, n, TCP_WRITE_FLAG_COPY) != ERR_OK) {
            LOG_INFO(TAG, "ERR tcp_write erreur"); return false;
        }
        p += n; len -= n;
    }
    return true;
}

// ─── Accueil Engine Driver : VN / HT / RL<n>]\[nom}|{adr}|{S|L... / PPA2 ────
void send_welcome_message(struct tcp_pcb *pcb) {
    static char rl[4 + ROSTER_MAX_ENTRIES * (ROSTER_STRING_MAX + 20) + 8];
    size_t n = 0;
    uint8_t count = roster.count;

    LOG_INFO(TAG, "Welcome ED. Roster count: %d", count);

    tcp_write_all(pcb, "VN2.0\n", 6);
    tcp_write_all(pcb, "HTWiBiDiB\n", 10);

    n += snprintf(rl + n, sizeof(rl) - n, "RL%d", count);
    for (uint8_t i = 0; i < count; i++) {
        const roster_entry_t *e = roster_get(i);
        if (!e) continue;
        n += snprintf(rl + n, sizeof(rl) - n, "]\\[%s}|{%d}|{%s",
                      e->id, e->dccAddress, e->longAddress ? "L" : "S");
        if (n >= sizeof(rl) - 2) { LOG_INFO(TAG, "ERR RL tronque"); n = sizeof(rl) - 2; break; }
    }
    rl[n++] = '\n';
    LOG_INFO(TAG, "RL len=%u sndbuf=%u : %.70s", (unsigned)n, (unsigned)tcp_sndbuf(pcb), rl);
    bool ok = tcp_write_all(pcb, rl, n);
    LOG_INFO(TAG, "RL envoye ok=%d", ok);

    // pas de "PW<port>" : on n'a pas de serveur HTTP de roster
    tcp_write_all(pcb, "PPA2\n", 5);
    tcp_output(pcb);
}


// ─── process_rx_withrottle() ─────────────────────────────────────────────────
void process_rx_withrottle(rx_data_t *data, uint8_t slot) {
    char actionData[30] = {};
    int  delimiter      = 0;

   // LOG_INFO(TAG, "rcv: <- slot %d message: %s", slot, data->msg);

    throttle[slot].state = NODE_LOGGED_ON;
    struct tcp_pcb *pcb  = data->pcb;

    // ── '*' heartbeat ─────────────────────────────────────────────────────────
    if (data->msg[0] == '*') {
        if (data->msg[1] == '+') heartbeatEnable[slot] = true;
        heartbeat[slot] = now_ms();
    }

    // ── 'N' device name ───────────────────────────────────────────────────────
    else if (data->msg[0] == 'N') {
        LOG_INFO(TAG, "N (device name) slot %d, heartbeat=%ds",
                 slot, heartbeatTimeout);
        char str[10];
        snprintf(str, sizeof(str), "*%d\n\n", heartbeatTimeout);
        send_msg(pcb, (int)strlen(str), str);
        tcp_output(pcb);
    }

    // ── 'H' device ID → envoyer accueil complet ───────────────────────────────
    else if (data->msg[0] == 'H') {
        LOG_INFO(TAG, "H (device ID) received -> sending VN/HT/RL/PPA");
        send_welcome_message(pcb);
    }

    // ── 'P' power ────────────────────────────────────────────────────────────
    else if (data->msg[0] == 'P') {
        if (data->msg[1] == 'P' && data->msg[2] == 'A') {
            char stri[10];
            snprintf(stri, sizeof(stri), "PPA%c\n\n", data->msg[3]);
            send_msg(pcb, (int)strlen(stri), stri);
            tcp_output(pcb);
            // Traduire PPA en BiDiB boost
            if (data->msg[3] == '1') {
                bidib_send_boost_state(1);   // MSG_BOOST_ON
            } else if (data->msg[3] == '0') {
                bidib_send_boost_state(0);   // MSG_BOOST_OFF
            }
        // PPA2 = état inconnu → on ne fait rien
        }
    }

    // ── 'M' multithrottle ────────────────────────────────────────────────────
    else if (data->msg[0] == 'M') {
      //  printf("[withrottle_if] case M\n");
        LOG_INFO(TAG," case M ");
        char th[3] = {};
        th[0] = data->msg[1];  // '0' pour Engine Driver

        // Trouver ';' dans "M0+S14<;>S14"
        delimiter = 0;
        for (int j = 0; j < data->len; j++) {
            if (data->msg[j] == ';') { 
                delimiter = j; 
                break;
            }
        }

        // LocoAdress : msg[4..delimiter-2]
        memset(LocoAdress_g, 0, sizeof(LocoAdress_g));
        for (int k = 4; k < (delimiter - 1); k++)
            LocoAdress_g[k - 4] = data->msg[k];

        // Slot ED : index direct (1 loco par slot)
        Throttle_g       = slot;
        Loco[slot].slot  = slot;

        uint8_t action = (uint8_t)data->msg[2];  // '+', '-', 'A'

        // cle de loco : msg[3 .. delimiter-2]  ex "L4528"
        memset(locoKey_g, 0, sizeof(locoKey_g));
        for (int k = 3; k < (delimiter - 1) && (k - 3) < (int)sizeof(locoKey_g) - 1; k++)
            locoKey_g[k - 3] = data->msg[k];

        // texte apres '<;>' : action (V/R/F/...) ou, pour '+', l'id roster
        memset(actionKey_g, 0, sizeof(actionKey_g));
        for (int k = (delimiter + 2); k < data->len && (k - (delimiter + 2)) < (int)sizeof(actionKey_g) - 1; k++) {
            actionKey_g[k - (delimiter + 2)] = data->msg[k];
        }

        // Nettoyer \n / \r final
        int aklen = (int)strlen(actionKey_g);
        while (aklen > 0 && (actionKey_g[aklen-1] == '\n' ||
                              actionKey_g[aklen-1] == '\r')) {
            actionKey_g[--aklen] = '\0';
        }

        // ── '+' : ajout loco ──────────────────────────────────────────────────
        if (action == '+') {
            int k = (int)strtol(LocoAdress_g, NULL, 10);
            Loco[slot].longAddr = (data->msg[3] == 'L');
            Loco[slot].dccAdress = k;
           // printf("[withrottle_if] locoAdd th=%s ak=%s dcc=%d slot=%d\n",
           //       th, actionKey_g, k, slot);
            LOG_INFO(TAG," loco+  key=%s ref=%s dcc=%d slot=%d", locoKey_g, actionKey_g, k, slot );
            
            locoAdd(th, locoKey_g, actionKey_g, slot);
        }

        // ── '-' : release loco ────────────────────────────────────────────────
        else if (action == '-') {
            locoRelease(th, locoKey_g, slot);
        }

        // ── 'A' : action ──────────────────────────────────────────────────────
        else if (action == 'A') {
            locoAction(th, actionKey_g, slot);
        }

    } // case 'M'

    // ── 'Q' : client quitte ───────────────────────────────────────────────────
    else if (data->msg[0] == 'Q') {
        LOG_INFO(TAG, "Q: client quit, slot %d", slot);
        throttle_stop(slot);
    }
/*
    // ── Vérification changement de vitesse ───────────────────────────────────
    if (Loco[slot].newSpeed != Loco[slot].oldSpeed) {
        Loco[slot].oldSpeed = Loco[slot].newSpeed;
        uint8_t active = 1;
        bidib_send_cs_drive(Loco[slot].dccAdress, Loco[slot].newSpeed, Loco[slot].dir,
             Loco[slot].LocoState, active);
    }
*/
    // ── Heartbeat watchdog ────────────────────────────────────────────────────
    if (heartbeatEnable[slot]) checkHeartbeat(slot);
}