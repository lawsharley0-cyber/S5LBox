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
    .equ MB_FIQ,    20

    /* Device pages the harness maps (tools/guest_bench.c). */
    .equ TIMER_VA,  0x00fe0000
    .equ VIC_VA,    0x00ff0000

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
    b   fiq_handler         @ 0x1c FIQ

    .text
    .global _start
_start:
    ldr     sp, =__svc_stack_top
    ldr     r4, =mbox
    ldr     r0, [r4, #MB_ID]
    ldr     r1, [r4, #MB_SCALE]
    ldr     r5, [r4, #MB_USER]
    /* Optional periodic timer FIQ, set up while still privileged: timer 4
     * reloads every MB_FIQ timebase ticks and VIC0 line 7 is routed to FIQ,
     * as XNU does. The handler touches only FIQ-banked registers and its own
     * counter, so every workload's checksum is unchanged by it -- while the
     * exact instruction at which each FIQ lands shows in LR_fiq/SPSR_fiq. */
    ldr     r6, [r4, #MB_FIQ]
    cmp     r6, #0
    beq     2f
    ldr     r7, =TIMER_VA
    str     r6, [r7, #0xa8]         @ TIMER4_COUNTBUF
    mov     r8, #3
    str     r8, [r7, #0xa4]         @ TIMER4_STATE = START | UPDATE
    ldr     r7, =VIC_VA
    mov     r8, #0x80
    str     r8, [r7, #0x0c]         @ VIC_INTSELECT: line 7 -> FIQ
    str     r8, [r7, #0x10]         @ VIC_INTENABLE
    cpsie   f
2:
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

fiq_handler:
    ldr     r8, =TIMER_VA
    ldr     r9, =0x00030000
    str     r9, [r8, #0xf8]         @ acknowledge (write-1-to-clear)
    ldr     r10, =fiq_counter
    ldr     r11, [r10]
    add     r11, r11, #1
    str     r11, [r10]
    subs    pc, lr, #4

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

    .ltorg
