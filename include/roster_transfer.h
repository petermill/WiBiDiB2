/*
 * roster_transfer.h  —  Pico 2W WiThrottle/BiDiB gateway
 *
 * Réception du roster envoyé par le CS via MSG_STRING_SET,
 * namespace BIDIB_STRING_NAMESPACE_ROSTER (chunks Base64 + en-tête).
 */
#ifndef ROSTER_TRANSFER_H_
#define ROSTER_TRANSFER_H_

#include <stdint.h>

#define BIDIB_STRING_NAMESPACE_ROSTER   250
// À traiter depuis le case MSG_STRING_SET de bidib_client_parser.c
// quand ns == BIDIB_STRING_NAMESPACE_ROSTER.
void roster_transfer_handle_chunk(uint8_t string_id, const uint8_t *data, uint8_t size);

#endif /* ROSTER_TRANSFER_H_ */