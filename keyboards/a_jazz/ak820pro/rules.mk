# The GC9107 panel + dashboard are driven entirely bare-metal (graphics/lcd_bus.c):
# we own SPI0 (+ Vector58) for the interrupt-driven flash->LCD DMA, and decode QP's
# qgf/qff asset blobs ourselves. No Quantum Painter runtime -- its concurrent main-loop
# activity corrupted the background DMA. See docs/LCD_FLASH_LAYER.md.
SRC   += graphics/lcd_bus.c

# CH582F wireless module exposed through QMK's official Bluetooth driver API.
# BLUETOOTH_DRIVER = custom defines BLUETOOTH_ENABLE, CONNECTION_ENABLE and
# NO_USB_STARTUP_CHECK and compiles bluetooth.c (weak bluetooth_* defaults);
# our ch582f_ajazz.c (below in SRC) provides the strong overrides.
BLUETOOTH_ENABLE = yes
BLUETOOTH_DRIVER = custom

# WIP: external PCF8563 RTC over the ChibiOS software (bit-banged) I2C fallback LLD.
# Swaps the SN32 HW I2C driver for the SW fallback; rtc.c drives it via the I2C HAL
# API. Does NOT work on hardware yet (compiles/links fine) -- see rtc.c.
USE_HAL_I2C_FALLBACK = yes

SRC += bluetooth/ch582f_ajazz.c

# Dashboard graphics now live in EXTERNAL FLASH, not firmware. Generate the blob
# with `python3 graphics/res/mkraw.py --flash` and upload it once per keyboard:
#   ak820ctl flash write 0x0CE0000 graphics/res/flash_assets.bin
# The firmware reads the index at boot (flash_assets_init) and DMA-draws by id.

SRC += graphics/display.c
SRC += rtc/rtc.c
SRC += watchdog.c   # hardware WDT: arm/kick/boot accounting/bootloader stop
SRC += health.c     # unified health counters; raw HID channel 0x13
SRC += kb_eeconfig.c # owner of the persisted kb datablock
SRC += bt_ui.c      # wireless slider, BT slot keys, hold-to-pair
VPATH += bluetooth
VPATH += graphics
VPATH += rtc

# Board-local RGB effects (rgb_matrix_kb.inc): RAINFALL
RGB_MATRIX_CUSTOM_KB = yes
