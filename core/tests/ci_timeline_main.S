        .syntax unified
        .arm
        .text
@ Main program at VA 0x10000, SVC mode.
start:
        msr cpsr_c, #0xd2       @ IRQ mode, masked
        mov sp, #0x23000
        msr cpsr_c, #0xd3       @ SVC mode, masked
        mov sp, #0x23800
        mov r3, #0x20000
        mov r1, #0x20400
        str r1, [r3]            @ log pointer
        mov r1, #0
        str r1, [r3, #4]        @ interrupt count
        mov r0, #0x26000        @ VIC0
        mov r1, #0x80           @ line 7: the timer
        str r1, [r0, #0x10]     @ INTENABLE
        mov r0, #0x27000        @ timer
        mov r1, #37
        str r1, [r0, #0xa8]     @ TIMER4_COUNTBUF
        mov r1, #3
        str r1, [r0, #0xa4]     @ TIMER4_STATE: start | update
        mov r10, #0
        mov r6, #1
        mov r7, #2
        mov r8, #0
        mov r9, #0
        cpsie i
main:
        and r5, r10, #0xff      @ a compute loop of 50..305 iterations
        add r5, r5, #50
2:      add r6, r6, r5
        eor r7, r7, r6, ror #3
        subs r5, r5, #1
        bne 2b
        tst r10, #3             @ every 4th: the 64-bit tick counter
        bne 3f
        mov r0, #0x27000
        ldr r1, [r0, #0x84]
        add r8, r8, r1
        ldr r2, [r0, #0x80]
        eor r8, r8, r2, ror #7
        ldr r1, [r0, #0xb4]     @ and the live down-count
        add r8, r8, r1, lsl #3
3:      tst r10, #15            @ every 16th: the VIC's raw lines
        bne 4f
        mov r0, #0x26000
        ldr r1, [r0, #0x08]
        add r9, r9, r1
4:      tst r10, #0x7f          @ every 128th: idle until the next interrupt
        bne 5f
        mov r0, #0
        mcr p15, 0, r0, c7, c0, 4
5:      tst r10, #0x300         @ every 1024 in 4: mask briefly, then unmask
        bne 6f
        cpsid i
        mov r1, #200
7:      subs r1, r1, #1
        bne 7b
        cpsie i
6:      add r10, r10, #1
        b main
