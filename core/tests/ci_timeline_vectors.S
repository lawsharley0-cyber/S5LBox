        .syntax unified
        .arm
        .text
@ Vector page at VA 0. Only IRQ is expected; the rest park.
vectors:
        b .                     @ 0x00 reset
        b .                     @ 0x04 undefined
        b .                     @ 0x08 SWI
        b .                     @ 0x0c prefetch abort
        b .                     @ 0x10 data abort
        b .                     @ 0x14 reserved
        b irq                   @ 0x18 IRQ
        b .                     @ 0x1c FIQ
        .space 0x100 - 0x20
@ IRQ handler at VA 0x100: acknowledge the timer, log the tick counter read
@ mid-handler and the main loop's progress, and every 8th interrupt
@ reprogram the period from the tick counter (23..86 timebase ticks).
irq:
        push {r0-r4, lr}
        mov r0, #0x27000        @ timer
        ldr r1, [r0, #0xf8]     @ IRQLATCH
        str r1, [r0, #0xf4]     @ IRQACK (write 1 to clear)
        ldr r2, [r0, #0x84]     @ TICKSLOW
        mov r3, #0x20000        @ data: [0] log pointer, [4] interrupt count
        ldr r4, [r3]
        str r2, [r4], #4
        str r10, [r4], #4
        cmp r4, #0x22000
        movhs r4, #0x20400
        str r4, [r3]
        ldr r1, [r3, #4]
        add r1, r1, #1
        str r1, [r3, #4]
        tst r1, #7
        bne 1f
        and r2, r2, #0x3f
        add r2, r2, #23
        str r2, [r0, #0xa8]     @ TIMER4_COUNTBUF: new period, loads live value
1:      pop {r0-r4, lr}
        subs pc, lr, #4
