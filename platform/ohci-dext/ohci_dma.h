// ohci_dma.h — 1394 OHCI DMA descriptor format + async context control (ported from ohci.c, GPL-2.0).
//
// OHCI async transmit/receive (AT/AR) run DMA "context programs" made of 16-byte descriptors. This
// header holds the descriptor struct + the control/context-register bits the dext's AT/AR code uses.
// All descriptor fields are little-endian in memory (the controller reads them LE). See spec 1394
// OHCI 1.1 §3 (DMA contexts) + §7-8 (async).
#ifndef UF_OHCI_DMA_H
#define UF_OHCI_DMA_H
#include <stdint.h>

// A single OHCI DMA descriptor (16 bytes, 16-byte aligned). Little-endian in memory.
struct ohci_descriptor {
    uint16_t req_count;
    uint16_t control;
    uint32_t data_address;
    uint32_t branch_address;   // next-descriptor address | Z (Z = descriptor count in next block)
    uint16_t res_count;
    uint16_t transfer_status;
} __attribute__((aligned(16)));

// descriptor.control command + option bits (ohci.c).
#define DESC_OUTPUT_MORE      0
#define DESC_OUTPUT_LAST      (1u << 12)
#define DESC_INPUT_MORE       (2u << 12)
#define DESC_INPUT_LAST       (3u << 12)
#define DESC_STATUS           (1u << 11)
#define DESC_KEY_IMMEDIATE    (2u << 8)
#define DESC_IRQ_ALWAYS       (3u << 4)
#define DESC_BRANCH_ALWAYS    (3u << 2)
#define DESC_WAIT             (3u << 0)

// Context control register bits (CONTROL_SET/CLEAR at the context base; ohci.c).
#define CTX_RUN     0x00008000
#define CTX_WAKE    0x00001000
#define CTX_DEAD    0x00000800
#define CTX_ACTIVE  0x00000400
// Register offsets within a context (base+): SET=0, CLEAR=4, COMMAND_PTR=12, MATCH=16.
#define CTX_CONTROL_SET(base)   (base)
#define CTX_CONTROL_CLEAR(base) ((base) + 4)
#define CTX_COMMAND_PTR(base)   ((base) + 12)

// Async context register bases (ohci.h). AT = transmit, AR = receive; Req/Rsp = request/response.
#define OHCI_AT_REQ_BASE  0x180
#define OHCI_AT_RSP_BASE  0x1A0
#define OHCI_AR_REQ_BASE  0x1C0
#define OHCI_AR_RSP_BASE  0x1E0

#endif /* UF_OHCI_DMA_H */
