/*
 * roster_store.h  —  WiBiDiB (Pico 2W)
 *
 * Persistance du roster dans la flash integree : on stocke le blob binaire recu
 * du CS (deja valide par CRC-8), pas la structure roster_t. Au demarrage on le
 * relit et on le passe a roster_load().
 */
#pragma once
#include <stdint.h>

// A appeler une fois au demarrage (apres l'init, avant la boucle principale).
void roster_store_init_load(void);

// A appeler apres un transfert reussi : memorise le blob ; l'ecriture en flash
// est differee (voir SAVE_DELAY_MS) et sautee si le contenu est identique.
void roster_store_request_save(const uint8_t *blob, uint32_t len);

// A appeler a chaque tour de la boucle principale.
void roster_store_poll(void);
