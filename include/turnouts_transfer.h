/*
 * turnouts_transfer.h  --  Pico 2W WiThrottle/BiDiB gateway
 *
 * Reception of the turnout list sent by the CS in MSG_STRING_SET chunks,
 * namespace BIDIB_STRING_NAMESPACE_TURNOUTS (same mechanism as roster_transfer.c).
 */
#ifndef TURNOUTS_TRANSFER_H_
#define TURNOUTS_TRANSFER_H_

#include <stdint.h>
#include <stdbool.h>
#include "turnouts_blob.h"

// To be called from the MSG_STRING_SET case of bidib_client_parser.c
// when ns == BIDIB_STRING_NAMESPACE_TURNOUTS.
void turnouts_transfer_handle_chunk(uint8_t string_id, const uint8_t *data, uint8_t size);

// Implemented in withrottle_if.c: replaces the table of turnouts announced to Engine Driver.
// Returns false (table unchanged) if the list is not usable.
bool withrottle_turnouts_set(const tb_entry_t *e, uint8_t count);

#endif /* TURNOUTS_TRANSFER_H_ */
