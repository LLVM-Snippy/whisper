# WFI and Timer Interrupts

This example waits in a supervisor-mode `wfi` loop until a machine-mode timer
interrupt arrives.

It opens a full-address-space PMP entry so S-mode can fetch and run, programs
the ACLINT `mtimecmp` register, and enables machine timer interrupts
(`mie.MTIE`). It then sets `mstatus.MPP` to supervisor and `mret`s into the
wait loop. The machine timer is not delegated, so the interrupt is taken in
M-mode even though `wfi` is executing in S-mode.

The MSTATUS.TW bit is set by the test to make WFI avoid taking an exception if it
completes within a bounded time (wfi_timeout).

The wfi_timeout is set to 20 in whisper.json. When the WFI instruction is executed, it
stalls until the timer expires and the timer interrupt becomes pending. The WFI
instruction does not trigger an exception. The subsequent instruction takes the timer
interrupt.

The trap handler checks that `mcause` is a machine timer interrupt (interrupt
bit set, code 7), that `mstatus.MPP` is supervisor, and that `mepc` points into
the wait loop. A store of 1 to `tohost` marks success.

Build and run:

```
make run
```
