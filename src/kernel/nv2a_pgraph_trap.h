/*
 * PGRAPH's software-method trap; see nv2a_pgraph_trap.c.
 */
#ifndef NV2A_PGRAPH_TRAP_H
#define NV2A_PGRAPH_TRAP_H

#include <stdint.h>

/* Trap NO_OPERATION with a non-zero parameter the way PGRAPH does: latch it,
 * raise the GPU interrupt, and wait until the title's handler has acknowledged
 * it (bounded). Returns 1 if the method was trapped, 0 if the trap is off or
 * cannot be delivered -- the caller then treats the method as unhandled. */
int nv2a_pgraph_trap_nop(uint32_t subch, uint32_t param);

#endif
