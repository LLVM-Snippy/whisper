#define mtime    0x0200bff8
#define mtimecmp 0x02004000

        .globl _start
_start:
        // Allow S-mode to execute and access memory.
        li      t0, 0x1f
        csrw    pmpcfg0, t0
        li      t0, -1
        csrw    pmpaddr0, t0

        // Direct-mode M-mode trap vector. The machine timer interrupt is
        // not delegated, so it is taken in M-mode even when WFI runs in S-mode.
        la      t0, timer_handler
        csrw    mtvec, t0

	// Set MSTATUS.TW
        li      t0, 1 << 21
        csrrs   x0, mstatus, t0

        // Enable machine-timer interrupts. In S-mode, M-mode interrupts are
        // taken regardless of mstatus.MIE, so MTIE is enough.
        li      t0, 1 << 7
        csrs    mie, t0

        // Program ACLINT mtimecmp. mtime starts at 0 and advances with each
        // retired instruction, so a compare of 100 expires while WFI is waiting.
        la      t0, mtimecmp
        li      t1, 30
        sd      t1, (t0)

        // mstatus.MPP = Supervisor, then mret into the WFI wait loop.
        li      t0, 0x1800
        csrc    mstatus, t0
        li      t0, 0x800
        csrs    mstatus, t0
        la      t0, wait
        csrw    mepc, t0
        mret

        // Runs in S-mode. WFI is a hint: if the timer has not yet fired,
        // fall through and wait again. The interrupt is taken with mepc at
        // wfi or at the following jump.
wait:
        wfi
        j       wait

fail:
        li      t0, 3
        sd      t0, tohost, t1
1:      j       1b

        .align  2
timer_handler:
        // Expect machine timer interrupt: interrupt bit + exception code 7.
        csrr    t0, mcause
        li      t1, 0x8000000000000007
        bne     t0, t1, fail

        // Trap must have come from S-mode (mstatus.MPP == 01).
        csrr    t0, mstatus
        srli    t0, t0, 11
        andi    t0, t0, 3
        li      t1, 1
        bne     t0, t1, fail

        // Interrupted instruction must be the S-mode WFI wait loop.
        csrr    t0, mepc
        la      t1, wait
        la      t2, fail
        bltu    t0, t1, fail
        bgeu    t0, t2, fail

        li      t0, 1
        sd      t0, tohost, t1
1:      j       1b

        .section .tohost
        .globl tohost
        .globl fromhost
tohost: .dword 0
fromhost: .dword 0
