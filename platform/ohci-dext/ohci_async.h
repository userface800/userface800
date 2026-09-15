// ohci_async.h — build an OHCI AT (async-transmit) descriptor block for a request packet.
//
// Ported from ohci.c at_context_queue_packet (GPL-2.0). An async request is programmed as a block
// of up to 4 descriptors: d[0] carries the KEY_IMMEDIATE control + the AT header inline in d[1..],
// and (for block/lock) d[2] is an OUTPUT_LAST pointing at the payload; the last descriptor gets
// OUTPUT_LAST | IRQ_ALWAYS | BRANCH_ALWAYS. Descriptors are LE in memory — on a little-endian host
// the struct fields ARE the wire values, so this is host-testable (see tests/test_ohci_atblock.cpp).
//
// The dext copies this block into its AT DMA buffer, patches branch_address for chaining, then
// writes CommandPtr = block_device_addr | Z and sets CTX_RUN/CTX_WAKE (see UFFireWireOHCI.cpp).
#ifndef UF_OHCI_ASYNC_H
#define UF_OHCI_ASYNC_H
#include <string.h>
#include "ohci_dma.h"

// Fill an AT descriptor block. `header`/`header_quads` = the OHCI AT header (3 or 4 quadlets, from
// uf::ohci::build_at_header). For block/lock requests pass the payload's device (DMA) address +
// length; for quadlet/read requests pass payload_len = 0. `d` must point at 4 zeroed descriptors.
// Returns Z — the descriptor count for the CommandPtr / branch_address low nibble (2 or 3).
static inline unsigned ohci_build_at_block(struct ohci_descriptor* d,
                                           const uint32_t* header, unsigned header_quads,
                                           uint32_t payload_addr, uint32_t payload_len)
{
    memset(d, 0, 4 * sizeof(*d));

    // d[0]: immediate-key control; req_count = header byte length (12 or 16). The header quadlets
    // live inline starting at d[1] (each descriptor is 16 bytes = 4 quadlets).
    d[0].control   = (uint16_t)DESC_KEY_IMMEDIATE;
    d[0].req_count = (uint16_t)(header_quads * 4);
    uint32_t* imm = (uint32_t*)&d[1];
    for (unsigned i = 0; i < header_quads; ++i) imm[i] = header[i];

    struct ohci_descriptor* last;
    unsigned z;
    if (payload_len > 0) {
        // Block/lock: d[2] is an OUTPUT_LAST descriptor pointing at the payload buffer.
        d[2].req_count    = (uint16_t)payload_len;
        d[2].data_address = payload_addr;
        last = &d[2];
        z = 3;
    } else {
        last = &d[0];
        z = 2;
    }

    last->control |= (uint16_t)(DESC_OUTPUT_LAST | DESC_IRQ_ALWAYS | DESC_BRANCH_ALWAYS);
    // branch_address is patched by the dext when chaining (address | Z of the next block); a Z of 0
    // in the low nibble terminates the program.
    return z;
}

#endif /* UF_OHCI_ASYNC_H */
