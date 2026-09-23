/*
 * S5LBox — guest benchmark image: vectors and startup (ARM state).
 *
 * The harness (tools/guest_bench.c) maps VA 0 upward onto DRAM with 4 KiB
 * pages, enters _start in SVC mode with IRQ/FIQ masked, and polls the mailbox.
 * _start optionally drops to User mode, calls wl_run(id, scale, arena,
 * &svc_counter), stores the result and raises `done`. Any unexpected exception
 * records its vector in `done` instead and parks.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
    .syntax unified
    .arm

    .equ MB_ID,     0
    .equ MB_SCALE,  4
    .equ MB_USER,   8
    .equ MB_RESULT, 12
    .equ MB_DONE,   16

    .section .vectors, "ax"
    .global _vectors
_vectors:
    b   _start              @ 0x00 reset
    b   fail_undef          @ 0x04 undefined instruction
    b   svc_handler         @ 0x08 SVC
    b   fail_pabt           @ 0x0c prefetch abort
    b   fail_dabt           @ 0x10 data abort
    b   fail_rsvd           @ 0x14 reserved
    b   fail_irq            @ 0x18 IRQ
    b   fail_fiq            @ 0x1c FIQ

    .text
    .global _start
_start:
    ldr     sp, =__svc_stack_top
    ldr     r4, =mbox
    ldr     r0, [r4, #MB_ID]
    ldr     r1, [r4, #MB_SCALE]
    ldr     r5, [r4, #MB_USER]
    cmp     r5, #0
    beq     1f
    cps     #0x10               @ User mode; interrupts stay masked
    ldr     sp, =__usr_stack_top
1:
    ldr     r2, =wl_arena
    ldr     r3, =svc_counter
    ldr     r12, =wl_run        @ bit 0 set when wl_run is Thumb
    blx     r12
    str     r0, [r4, #MB_RESULT]
    mov     r0, #1
    str     r0, [r4, #MB_DONE]
done_loop:
    b       done_loop

/* The SVC workload's handler: count and return to the caller's state. */
svc_handler:
    push    {r0, r1}
    ldr     r0, =svc_counter
    ldr     r1, [r0]
    add     r1, r1, #1
    str     r1, [r0]
    pop     {r0, r1}
    movs    pc, lr

    .macro FAIL name, code
\name:
    ldr     r4, =mbox
    ldr     r0, =\code
    str     r0, [r4, #MB_DONE]
1:  b       1b
    .endm

    FAIL fail_undef, 0xf04
    FAIL fail_pabt,  0xf0c
    FAIL fail_dabt,  0xf10
    FAIL fail_rsvd,  0xf14
    FAIL fail_irq,   0xf18
    FAIL fail_fiq,   0xf1c

    .ltorg
