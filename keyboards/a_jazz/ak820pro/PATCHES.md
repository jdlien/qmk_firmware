# ChibiOS patches — the submodule branch is now authoritative

As of 2026-09-01 the hand-applied ChibiOS patches are **committed** in
`lib/chibios-contrib` on the local branch **`ak820pro-patches`** (seven
commits on top of the upstream pin `5bed8690`), and the superproject gitlink
pins that branch's tip. `git submodule update` can no longer silently
destroy them — it will detach to the pinned (patched) commit instead.

The `.diff` files in this directory are kept as documentation and as the
recovery path. There are **seven**, not the six older docs mention —
`spi_dma_abort.diff` was added 2026-08-31 with the blit-timeout work.
Apply order (each `git apply` from the chibios-contrib root):

```
hardware_pwm -> i2c_fallback -> rtc_lld -> spi_fifo_pump
             -> spi_flash_dma -> spi_dma_abort -> efl_ramtext
```

`spi_fifo_pump` / `spi_flash_dma` / `spi_dma_abort` touch the same SPI LLD
files and must stay in that order; `efl_ramtext` is required for VIA.

Recovery if the branch is ever lost: a git bundle of it lives at
`ak820pro-builds/chibios-ak820pro-patches.bundle`
(`git fetch <bundle> ak820pro-patches`), and the flattened applied state at
`ak820pro-builds/chibios-applied-state-backup-2026-09-01.diff`.

The branch is pushed to https://github.com/jdlien/ChibiOS-Contrib
(`ak820pro-patches`), which `.gitmodules` now points at, so a fresh clone's
`git submodule update` fetches the pinned patched commit directly.
