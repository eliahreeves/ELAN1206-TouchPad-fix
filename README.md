# ELAN1206 Linux TouchPad fix

Keeps the ELAN1206 I2C touchpad usable on Linux 6.9 and newer. Written for the
ASUS ZenBook UX564EH_Q528EH; it should also apply to other laptops whose
touchpad sits behind an Intel LPSS I2C controller (`i2c_designware.N`).

## Problem

Since kernel 6.9 the touchpad is detected and clicks work, but the cursor
barely moves: reports arrive only about 10 times per second, and libinput
sees a touch-up right after every touch-down. `dmesg` shows

    irq 28: nobody cared (try booting with the "irqpoll" option)
    handlers:
    [<...>] i2c_dw_isr
    Disabling IRQ #28

## Cause

On this hardware the interrupt line shared by the touchpad's I2C controller
(`i2c_designware.1`) and its DMA engine (`idma64.1`) is stuck asserted and
fires tens of thousands of times per second. Before 6.9 the idma64 handler
silently claimed those interrupts, so nothing was logged and the touchpad
worked (at the cost of one CPU thread). Kernel commit
[9140ce47872b](https://git.kernel.org/linus/9140ce47872bfd89fca888c2f992faa51d20c2bc)
("dmaengine: idma64: Don't try to serve interrupts when device is powered
off") made the handler return `IRQ_NONE`, so the kernel now sees an unhandled
storm, disables the line, and the I2C controller is serviced only by the
kernel's 10 Hz spurious-IRQ poller.

A revert was [posted to LKML](https://lkml.iu.edu/hypermail/linux/kernel/2502.2/06587.html)
in February 2025 but did not land. See kernel bugzilla 217701 and 219799 and the
[Arch forum thread](https://bbs.archlinux.org/viewtopic.php?id=303199).
There is no BIOS fix: version 313 (2022) is the last ASUS released.

## How this works

Re-binding `idma64.1` re-enables the disabled interrupt (the kernel's
shared-IRQ setup path clears the spurious-disabled state). A device can only
be bound after it has been unbound, so `touchpad-monitor` runs this cycle:

* after 1 second without touch events, unbind `idma64.1`;
* on the next touch event, bind it again: the interrupt comes back and the
  touchpad is fully responsive.

While the touchpad is in use the controller handles just enough real I2C
interrupts that the kernel keeps the line enabled. Once the touchpad goes idle
every interrupt is unhandled again and the kernel disables the line within a
few seconds, whether or not `idma64.1` is bound. The idle timeout therefore
does not change the CPU cost; a longer timeout only delays the re-bind, which
leaves the touchpad laggy after a pause. That is why the default is short.

Cost: while the touchpad is in use one CPU thread services roughly 45,000
interrupts per second. On the UX564EH that adds up to about an hour of CPU
time per day of normal use.

The program finds the touchpad by its input device name, derives the DMA
device from the touchpad's sysfs path, waits for the device to appear at
boot, re-discovers it if it disappears (for example after suspend), and
re-binds `idma64.1` when stopped so the system is left in its stock state.
While the touchpad is in use it only checks for activity once per idle
period rather than waking on every report, so it stays off the CPU that is
busy with the interrupt storm. Note that `top` can still show it using CPU:
without `CONFIG_IRQ_TIME_ACCOUNTING` the kernel bills interrupt time to
whatever task happens to be running on that CPU.

## Install

    make
    sudo make install
    sudo systemctl enable --now touchpad-monitor

Check that it found the right devices:

    touchpad-monitor -l
    journalctl -u touchpad-monitor -b

Remove with `sudo make uninstall`.

## Options

| Flag | Meaning | Default |
|------|---------|---------|
| `-t SECONDS` | idle time before unbinding the DMA device | `1.0` |
| `-n NAME` | substring of the touchpad's input device name | `ELAN1206` |
| `-d DEVICE` | DMA platform device to toggle | derived, else `idma64.1` |
| `-e PATH` | event device to read instead of auto-detecting | |
| `-v` | log every bind and unbind to the journal | off |
| `-D` | dry run: never write to sysfs | off |
| `-l` | print the detected touchpad and DMA device, then exit | |

To use `-v` or another timeout with systemd, edit the `ExecStart` line with
`sudo systemctl edit --full touchpad-monitor`.
