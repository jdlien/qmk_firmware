# ChibiOS patches — the submodule branch is now authoritative

As of 2026-09-01 the hand-applied ChibiOS patches are **committed** in
`lib/chibios-contrib` on the local branch **`ak820pro-patches`** (now nine
commits on top of the upstream pin `5bed8690`: the seven below, then the
`SN32F290.ld` `.ram7` reservation for the watchdog record, then
`spi0_dispatch`), and the superproject gitlink pins that branch's tip. `git submodule update` can no longer silently
destroy them — it will detach to the pinned (patched) commit instead.

The `.diff` files in this directory are kept as documentation and as the
recovery path. There are **eight**: `spi_dma_abort.diff` was added
2026-08-31 with the blit-timeout work, and `spi0_dispatch.diff` 2026-09-23
(the SPI0 handler routes on the DMA state, not the raw flags, and clears
only the DMA flags it read -- a lost-completion race and a hijacked FIFO
interrupt, found in the crash hunt's review). The `.ram7` linker commit has
no `.diff` here; take it from the branch or the bundle.
Apply order (each `git apply` from the chibios-contrib root):

```
hardware_pwm -> i2c_fallback -> rtc_lld -> spi_fifo_pump
             -> spi_flash_dma -> spi_dma_abort -> efl_ramtext
             -> (SN32F290.ld .ram7 commit) -> spi0_dispatch
```

`spi_fifo_pump` / `spi_flash_dma` / `spi_dma_abort` / `spi0_dispatch` touch
the same SPI LLD file and must stay in that order; `efl_ramtext` is required
for VIA.

Recovery if the branch is ever lost: a git bundle of it lives at
`ak820pro-builds/chibios-ak820pro-patches.bundle`
(`git fetch <bundle> ak820pro-patches`), and the flattened applied state at
`ak820pro-builds/chibios-applied-state-backup-2026-09-01.diff`.

The branch is pushed to https://github.com/jdlien/ChibiOS-Contrib
(`ak820pro-patches`), which `.gitmodules` now points at, so a fresh clone's
`git submodule update` fetches the pinned patched commit directly.
