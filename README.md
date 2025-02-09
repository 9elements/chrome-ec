# Embedded Controller (EC)

[TOC]

## Introduction

The Chromium OS project includes open source software for embedded controllers
(EC) used in recent ARM and x86 based Chromebooks. This software includes a
lightweight, multitasking OS with modules for power sequencing, keyboard
control, thermal control, battery charging, and verified boot. The EC software
is written in C and currently supports two different ARM Cortex based
controllers. Intel based designs, such as the Chromebook Pixel use the TI
Stellaris LM4F (Cortex M4) while the Samsung Chromebook (XE303C12) and HP
Chromebook 11 use an ST-Micro STM32F100 (Cortex M3). Some STM32L variants are
also supported. Support for additional embedded controllers is ongoing.

This document is a guide to help make you familiar with the EC code, current
features, and the process for submitting code patches.

For more see the Chrome OS Embedded Controller
[presentation](https://docs.google.com/presentation/d/1Xa_Z6SjW-soPvkugAR8__TEJFrJpzoZUa9HNR14Sjs8/pub?start=false&loop=false&delayms=3000)
and [video](http://youtu.be/Ie7LRGgCXC8) from the
[2014 Firmware Summit](http://dev.chromium.org/chromium-os/2014-firmware-summit).

## What you will need

1.  A Chromebook with a compatible EC. This includes the Samsung Chromebook
    (XE303C12) and all Chromebooks shipped after the Chromebook Pixel 2013
    (inclusive). See the
    [Chrome OS devices](http://dev.chromium.org/chromium-os/developer-information-for-chrome-os-devices)
    page for a list.
1.  A Linux development environment. Ubuntu 14.04 Trusty (x86_64) is well
    supported. Linux in a VM may work if you have a powerful host machine.
1.  A [servo debug board](http://dev.chromium.org/chromium-os/servo) (and
    header) is highly recommended for serial console and JTAG access to the EC.
1.  A sense of adventure!

## Getting the EC code

The code for the EC is open source and is included in the Chromium OS
development environment (`~/trunk/src/platform/ec/</code>`).
See[ http://www.chromium.org/chromium-os/quick-start-guide](http://dev.chromium.org/chromium-os/quick-start-guide)
for build setup instructions. If you want instant gratification, you can fetch
the source code directly. However, you will need the tool-chain provided by the
Chromium OS development environment to build a binary.

```bash
git clone https://chromium.googlesource.com/chromiumos/platform/ec
```

The source code can also be broswed on the web at:

https://chromium.googlesource.com/chromiumos/platform/ec/

## Code Overview

The following is a quick overview of the top-level directories in the EC
repository:

**board** - Board specific code and configuration details. This includes the
GPIO map, battery parameters, and set of tasks to run for the device.

**build** - Build artifacts are generated here. Be sure to delete this and
rebuild when switching branches and before "emerging" (see Building an EC binary
below). make clobber is a convenient way to clean up before building.

**chip** - IC specific code for interfacing with registers and hardware blocks
(adc, jtag, pwm, uart etc…)

**core** - Lower level code for task and memory management.

**common** - A mix of upper-level code that is shared across boards. This
includes the charge state machine, fan control, and the keyboard driver (among
other things).

**driver** - Low-level drivers for light sensors, charge controllers,
I2C/onewire LED controllers, and I2C temperature sensors.

**include** - Header files for core and common code.

**util** - Host utilities and scripts for flashing the EC. Also includes
“ectool” used to query and send commands to the EC from userspace.

**test** - Unit tests for the EC. Use “make tests -j $jobs BOARD=$board” to run
them against your build target. Set $jobs to the number of cores in your build
machine. Please contribute new tests if writing new functionality.

## Firmware Branches

Each Chrome device has a firmware branch created when the read-only firmware is
locked down prior to launch. This is done so that updates can be made to the
read-write firmware with a minimal set of changes from the read-only. Some
Chrome devices only have build targets on firmware branches and not on
cros/master. Run “`git branch -a | grep firmware`” to locate the firmware branch
for your board. Note that for devices still under development, the board
configuration may be on the branch for the platform reference board.

To build EC firmware on a branch, just check it out and build it:

```bash
git checkout cros/firmware-falco_peppy-4389.B
```

To make changes on a branch without creating a whole new development environment
(chroot), create a local tracking branch:

```bash
git branch --track firmware-falco_peppy-4389.B cros/firmware-falco_peppy-4389.B

git checkout firmware-falco_peppy-4389.B

make clobber

# <make changes, test, and commit them>

repo upload --cbr .

# (The --cbr means "upload to the current branch")
```

Here is a useful command to see commit differences between branches (change the
branch1...branch2 as needed):

```bash
git log --left-right --graph --cherry-pick --oneline branch1...branch2
```

For example, to see the difference between cros/master and the HEAD of the
current branch:

```bash
git log --left-right --graph --cherry-pick --oneline cros/master...HEAD

# Note: Use three dots “...” or it won’t work!
```

## Building an EC binary

Note: The EC is normally built from within the Chromium OS development chroot to
use the correct toolchain.

Building directly from the EC repository:

```bash
cros_sdk
cd ~/trunk/src/platform/ec
make -j BOARD=<boardname>
```

Where **<boardname>** is replaced by the name of the board you want to build an
EC binary for. For example, the boardname for the Chromebook Pixel is “link”.
The make command will generate an EC binary at `build/<boardname>/ec.bin`. The
`-j` tells make to build multi-threaded which can be much faster on a multi-core
machine.

### Building via emerge (the build file used when you build Chrome OS):

(optional) Run this command if you want to build from local source instead of
the most recent stable version:

```bash
cros_workon-<boardname> start chromeos-ec
```

Build the EC binary:

```
emerge-<boardname> chromeos-ec
```

Please be careful if doing both local `make`s and running emerge. The emerge can
pick up build artifacts from the build subdirectory. It’s best to delete the
build directory before running emerge with `make clobber`.

The generated EC binary from emerge is found at:

```
(chroot) $ /build/<boardname>/firmware/ec.bin
```

The ebuild file used by Chromium OS is found
[here](https://chromium.googlesource.com/chromiumos/overlays/chromiumos-overlay/+/master/chromeos-base/chromeos-ec/chromeos-ec-9999.ebuild):

```bash
(chroot) $ ~/trunk/src/third_party/chromiumos-overlay/chromeos-base/chromeos-ec/chromeos-ec-9999.ebuild
```

## Flashing an EC binary to a board

### Flashing via the servo debug board

If you get an error, you may not have set up the dependencies for servo
correctly. The EC (on current Chromebooks) must be powered either by external
power or a charged battery for re-flashing to succeed. You can re-flash via
servo even if your existing firmware is bad.

```bash
(chroot) $ sudo emerge openocd
```

```bash
(chroot) $ ~/trunk/src/platform/ec/util/flash_ec --board=<boardname> [--image=<path/to/ec.bin>]
```

Note: This command will fail if write protect is enabled.

If you build your own EC firmware with the `make BOARD=<boardname>` command the
firmware image will be at:

```bash
(chroot) $ ~/trunk/src/platform/ec/build/<boardname>/ec.bin
```

If you build Chrome OS with `build_packages` the firmware image will be at:

```bash
(chroot) $ /build/<boardname>/firmware/ec.bin
```

Specifying `--image` is optional. If you leave off the `--image` argument, the
`flash_ec` script will first look for a locally built `ec.bin` followed by one
generated by `emerge`.

### Flashing on-device via flashrom

Assuming your devices boots, you can flash it using the `flashrom` utility. Copy
your binary to the device and run:

```bash
(chroot) $ flashrom -p ec -w <path-to/ec.bin>
```

Note: `-p internal:bus=lpc` also works on x86 boards...but why would you want to
remember and type all that?

## Preventing the RW EC firmware from being overwritten by Software Sync at boot

A feature called "Software Sync" keeps a copy of the read-write (RW) EC firmware
in the RW part of the system firmware image. At boot, if the RW EC firmware
doesn't match the copy in the system firmware, the EC’s RW section is
re-flashed. While this is great for normal use as it makes updating the EC and
system firmware a unified operation, it can be a challenge for EC firmware
development. To disable software sync a flag can be set in the system firmware.
Run the following commands from a shell on the device to disable Software Sync
and turn on other developer-friendly flags (note that write protect must be
disabled for this to work):

```bash
(chroot) $ /usr/share/vboot/bin/set_gbb_flags.sh 0x239
```

```bash
(chroot) $ reboot
```

This turns on the following flags:

*   `GBB_FLAG_DEV_SCREEN_SHORT_DELAY`
*   `GBB_FLAG_FORCE_DEV_SWITCH_ON`
*   `GBB_FLAG_FORCE_DEV_BOOT_USB`
*   `GBB_FLAG_DISABLE_FW_ROLLBACK_CHECK`
*   `GBB_FLAG_DISABLE_EC_SOFTWARE_SYNC`

The `GBB` (Google Binary Block) flags are defined in the
[vboot_reference source](https://chromium.googlesource.com/chromiumos/platform/vboot_reference/+/master/firmware/include/gbb_header.h).
A varying subset of these flags are implemented and/or relevant for any
particular board.

## Using the EC serial console

The EC has an interactive serial console available only through the UART
connected via servo. This console is essential to developing and debugging the
EC.

Find the serial device of the ec console (on your workstation):

```bash
(chroot) $ dut-control | grep ec_uart
```

Connect to the console:

```bash
(chroot) $ socat READLINE /dev/pts/XX
```

Where `XX` is the device number. Use `cu`, `minicom`, or `screen` if you prefer
them over `socat`.

### Useful EC console commands:

**help** - get a list of commands. help <command> to get help on a specific
command.

**chan** - limit logging message to specific tasks (channels). Useful if you’re
looking for a specific error or warning and don’t want spam from other tasks.

**battfake** - Override the reported battery charge percentage. Good for testing
low battery conditions (LED behavior for example). Set “battfake -1” to go back
to the actual value.

**fanduty** - Override automatic fan control. “fanduty 0” turns the fan off.
“autofan” switches back to automated control.

**hcdebug** - Display the commands that the host sends to the EC, in varying
levels of detail (see include/ec_commands.h for the data structures).

## Host commands

The way in which messages are exchanged between the AP and EC is
[documented separately](http://dev.chromium.org/chromium-os/ec-development/ap-ec-communication).

## Software Features

### Tasks

Most code run on the EC after initialization is run in the context of a task
(with the rest in interrupt handlers). Each task has a fixed stack size and
there is no heap (malloc). All variable storage must be explicitly declared at
build-time. The EC (and system) will reboot if any task has a stack overflow.
Tasks typically have a top-level loop with a call to task_wait_event() or
usleep() to set a delay in uSec before continuing. A watchdog will trigger if a
task runs for too long. The watchdog timeout varies by EC chip and the clock
speed the EC is running at.

The list of tasks for a board is specified in ec.tasklist in the `board/$BOARD/`
sub-directory. Tasks are listed in priority order with the lowest priority task
listed first. A task runs until it exits its main function or puts itself to
sleep. The highest priority task that wants to run is scheduled next. Tasks can
be preempted at any time by an interrupt and resumed after the handler is
finished.

The console `taskinfo` command will print run-time stats on each task:

```
> taskinfo
Task Ready Name         Events      Time (s)  StkUsed
   0 R << idle >>       00000000   32.975554  196/256
   1 R HOOKS            00000000    0.007835  192/488
   2   VBOOTHASH        00000000    0.042818  392/488
   3   POWERLED         00000000    0.000096  120/256
   4   CHARGER          00000000    0.029050  392/488
   5   CHIPSET          00000000    0.017558  400/488
   6   HOSTCMD          00000000    0.379277  328/488
   7 R CONSOLE          00000000    0.042050  348/640
   8   KEYSCAN          00000000    0.002988  292/488
```

The `StkUsed` column reports the largest size the stack for each task grew since
reset (or sysjump).

### Hooks

Hooks allow you to register a function to be run when specific events occur;
such as the host suspending or external power being applied:

```
DECLARE_HOOK(HOOK_AC_CHANGE, ac_change_callback, HOOK_PRIO_DEFAULT);
```

Registered functions are run in the HOOKS task. Registered functions are called
in priority order if more than one callback needs to be run. There are also
hooks for running functions periodically: `HOOK_TICK` (fires every
`HOOK_TICK_INVERVAL` ms which varies by EC chip) and `HOOK_SECOND`. See
hook_type in
[include/hooks.h](https://chromium.googlesource.com/chromiumos/platform/ec/+/master/include/hooks.h)
for a complete list.

### Deferred Functions

Deferred functions allow you to call a function after a delay specified in uSec
without blocking. Deferred functions run in the HOOKS task. Here is an example
of an interrupt handler. The deferred function allows the handler itself to be
lightweight. Delaying the deferred call by 30 mSec also allows the interrupt to
be debounced.

```
static int debounced_gpio_state;

static void some_interrupt_deferred(void)
{

        int gpio_state = gpio_get_level(GPIO_SOME_SIGNAL);

        if (gpio_state == debounced_gpio_state)
                return;

        debounced_gpio_state = gpio_state;

        dispense_sandwich(); /* Or some other useful action. */
}

/* A function must be explicitly declared as being deferrable. */
DECLARE_DEFERRED(some_interrupt_deferred);

void some_interrupt(enum gpio_signal signal)
{
        hook_call_deferred(some_interrupt_deferred, 30 * MSEC);
}
```

### Shared Memory Buffer

While there is no heap, there is a shared memory buffer that can be borrowed
temporarily (ideally before a context switch). The size of the buffer depends on
the EC chip being used. The buffer can only be used by one task at a time. See
[common/shared_mem.c](https://chromium.googlesource.com/chromiumos/platform/ec/+/master/common/shared_mem.c)
for more information. At present (May 2014), this buffer is only used by debug
commands.

## Making Code Changes

If you see a bug or want to make an improvement to the EC code please file an
issue at [crbug.com/new](http://crbug.com/new). It's best to discuss the change
you want to make first on an issue report to make sure the EC maintainers are
on-board before digging into the fun part (writing code).

In general, make more, smaller changes that solve single problems rather than
bigger changes that solve multiple problems. Smaller changes are easier and
faster to review. When changing common code shared between boards along with
board specific code, please split the shared code change into its own change
list (CL). The board specific CL can depend on the shared code CL.

### Coding style

The EC code follows the
[Linux Kernel style guide](https://www.kernel.org/doc/html/latest/process/coding-style.html).
Please adopt the same style used in the existing code. Use tabs, not spaces, 80
column lines etc...

Other style notes:

1.  Globals should either be `static` or `const`. Use them for persistent state
    within a file or for constant data (such as the GPIO list in board.c). Do
    not use globals to pass information between modules without accessors. For
    module scope, accessors are not needed.
1.  If you add a new `#define` config option to the code, please document it in
    [include/config.h](https://chromium.googlesource.com/chromiumos/platform/ec/+/master/include/config.h)
    with an `#undef` statement and descriptive comment.
1.  The Chromium copyright header must be included at the top of new files in
    all contributions to the Chromium project:

    ```
    /* Copyright <year> The Chromium OS Authors. All rights reserved.
     * Use of this source code is governed by a BSD-style license that can be
     * found in the LICENSE file.
     */
    ```

### Submitting changes

Prior to uploading a new change for review, please run the EC unit tests with:

```bash
(chroot) $ make -j buildall
```

```bash
(chroot) $ make -j tests
```

These commands will build and run unit tests in an emulator on your host.

Pre-submit checks are run when you try to upload a change-list. If you wish to
run these checks manually first, commit your change locally then run the
following command from within the chroot and while in the `src/platform/ec`
directory:

```bash
(chroot) $ ~/trunk/src/repohooks/pre-upload.py
```

The pre-submit checks include checking the commit message. Commit messages must
have a `BUG`, `BRANCH`, and `TEST` line along with `Signed-off-by: First Last
<name@company.com>`. The signed-off-by line is a statement that you have written
this code and it can be contributed under the terms of the `LICENSE` file.

Please refer to existing commits (`git log`) to see the proper format for the
commit message. If you have configured git properly, running `git commit` with
the `-s` argument will add the Signed-off-by line for you.

## Debugging

While adding `printf` statements can be handy, there are some other options for
debugging problems during development.

### Serial Console

There may already be a message on the serial console that indicates your
problem. If you don’t have a servo connected, the `ectool console` command will
show the current contents of the console buffer (the buffer’s size varies by EC
chip). This log persists across warm resets of the host but is cleared if the EC
resets. The `ectool console` command will only work when the EC is not write
protected.

If you have interactive access to the serial console via servo, you can use the
read word `rw` and write word `ww` commands to peek and poke the EC's RAM. You
may need to refer to the datasheet for your EC chip or the disassembled code to
find the memory address you need. There are other handy commands on the serial
console to read temperatures, view the state of tasks (taskinfo) which may help.
Type `help` for a list.

### Panicinfo

The EC may save panic data which persists across resets. Use the “`ectool
panicinfo`” command or console “`panicinfo`” command to view the saved data:

```
Saved panic data: (NEW)
=== HANDLER EXCEPTION: 05 ====== xPSR: 6100001e ===
r0 :00000001 r1 :00000f15 r2 :4003800c r3 :000000ff
r4 :ffffffed r5 :00000799 r6 :0000f370 r7 :00000000
r8 :00000001 r9 :00000003 r10:20002fe0 r11:00000000
r12:00000008 sp :20000fd8 lr :000012e1 pc :0000105e
```

The most interesting information are the program counter (`pc`) and the link
register (return address, `lr`) as they give you an indication of what code the
EC was running when the panic occurred. `HANDLER EXCEPTIONS` indicate the panic
occurred while servicing an interrupt. `PROCESS EXCEPTIONS` occur in regular
tasks. If you see “Imprecise data bus error” listed, the program counter value
is incorrect as the panic occurred when flushing a write buffer. If using a
cortex-m based EC, add `CONFIG_DEBUG_DISABLE_WRITE_BUFFER` to your board.h to
disable write buffering (with a performance hit) to get a “Precise bus error”
with an accurate program counter value.

### Assembly Code

If you have a program counter address you need to make sense of, you can
generate the assembly code for the EC by checking out the code at the matching
commit for your binary (`ectool version`) and running:

```bash
(chroot) $ make BOARD=$board dis
```

This outputs two files with assembly code:

```
build/$board/RO/ec.RO.dis
build/$board/RW/ec.RW.dis
```

which (in the case of the LM4 and STM32) are essentially the same, but the RW
addresses are offset.

## Write Protect

The EC has read-only (RO) and read-write (RW) firmware. Coming out of reset, the
EC boots into its RO firmware. The RO firmware boots the host and asks it verify
a hash of the RW firmware (software sync). If the RW firmware is invalid, it is
updated from a copy in the hosts RW firmware. Once the EC RW firmware is valid,
the EC jumps to it (without rebooting). The RO firmware is locked in the factory
and is never changed. The RW firmware can be updated later by pushing a new
system firmware containing an updated EC RW region.

Note that both the RO and RW firmware regions are normally protected once write
protect has been turned on. The RW region is unprotected at EC boot until it has
been verified by the host. The RW region is protected before the Linux kernel is
loaded.

### Hardware Write Protect

A hardware-based mechanism is used to prevent the RO firmware from being
changed. The most common design is to have an input grounded by a screw. When
the screw is inserted, hardware write protect is enabled. This grounded signal
can be read by the host chipset and EC. It is also routed to the “write protect”
pin on any SPI flash chips containing firmware.

### Software Write Protect

Software-based write protect state stored in non-volatile memory. If hardware
write protect is enabled, software write protect can be enabled but can’t be
disabled. If hardware write protect is disabled, software write protect can be
enabled or disabled (note that some implementations require an EC reset to
disable software write protect).

The underlying mechanism implementing software write protect may differ between
EC chips. However the common requirements are that software write protect can
only be disabled when hardware write protect is off and that the RO firmware
must be protected before jumping to RW firmware if protection is enabled.

### `ectool`

`ectool` includes commands to enable and disable software write protect.

#### `ectool flashprotect`

Print out current flash protection state.

```
Flash protect flags: 0x0000000f wp_gpio_asserted ro_at_boot ro_now all_now
Valid flags:         0x0000003f wp_gpio_asserted ro_at_boot ro_now all_now STUCK INCONSISTENT
Writable flags:      0x00000000
```

`Flash protect flags` - Current flags that are set.

`Valid flags` - All the options for flash protection.

`Writable flags` - The flags that currently can be changed. (In this case, no
flags can be changed).

Flags:

*   `wp_gpio_asserted` - Whether the hardware write protect GPIO is currently
    asserted (read only).

*   `ro_at_boot` - Whether the EC will write protect the RO firmware on the next
    boot of the EC.

*   `ro_now` - Protect the read-only portion of flash immediately. Requires
    hardware WP be enabled.

*   `all_now` - Protect the entire flash (including RW) immediately. Requires
    hardware WP be enabled.

*   `STUCK` - Flash protection settings have been fused and can’t be cleared
    (should not happen during normal operation. Read only.)

*   `INCONSISTENT` - One or more banks of flash is not protected when it should
    be (should not happen during normal operation. Read only.).

#### `ectool flashprotect enable`

Set `ro_at_boot` flag. The next time the EC is reset it will protect the flash.
Note that this requires a cold reset.

#### `ectool flashprotect enable now`

Set `ro_at_boot` `ro_now all_now` flags and immediately protect the flash. Note
that this will fail if hardware write protect is disabled.

#### `ectool flashprotect disable`

Clear `ro_at_boot` flag. This can only be cleared if the EC booted without
hardware write protect enabled.

Note that you must reset the EC to clear write protect after removing the screw.
If the `ro_at_boot` flag set and the EC resets with the HW gpio disabled, the EC
will leave the flash unprotected (`ro_now` and `all_now` flags are not set) but
leave `ro_at_boot` flag set.

### Flashrom

Flashrom can also be used to query and enable/disable
[EC flash protection](http://dev.chromium.org/chromium-os/firmware-porting-guide/firmware-ec-write-protection).

#### View the current state of flash protection

```bash
(chroot) $ flashrom -p ec --wp-status
```

```
WP: status: 0x00
WP: status.srp0: 0
WP: write protect is disabled.
WP: write protect range: start=0x00000000, len=0x00000000
```

#### Enable protection

This is immediate. The protection range indicates the RO region of the firmware.

```bash
(chroot) $ flashrom -p ec --wp-enable
```

```
SUCCESS
```

```bash
(chroot) $ flashrom -p ec --wp-status
```

```
WP: status: 0x80
WP: status.srp0: 1
WP: write protect is enabled.
WP: write protect range: start=0x00000000, len=0x0001f800
```

#### Disable protection

Disable can only be done with hardware write protect disabled.

```bash
(chroot) $ flashrom -p ec --wp-disable
```

```
FAILED: RO_AT_BOOT is not clear.
FAILED
```

Reboot with screw removed. Note that protection is still enabled but the
protection range is zero.

```bash
(chroot) $ flashrom -p ec --wp-status
```

```
WP: status: 0x80
WP: status.srp0: 1
WP: write protect is enabled.
WP: write protect range: start=0x00000000, len=0x00000000
```

```bash
(chroot) $ flashrom -p ec --wp-disable
```

```
SUCCESS
```

## EC Version Strings

The read-only and read-write sections of the EC firmware each have a version
string. This string tells you the branch and last change at which the firmware
was built. On a running machine, run `ectool version` from a shell to see
version information:

```
RO version:    peppy_v1.5.103-7abb4f7
RW version:    peppy_v1.5.129-cd1a1e9
Firmware copy: RW
Build info:    peppy_v1.5.129-cd1a1e9 2014-03-07 17:18:27 @build120-m2
```

You can also run the `version` command on the EC serial console for a similar
output.

The format of the version string is:

```
<board>_<branch number>.<number of commits since the branch tag was created>-<git hash of most recent change>
```

If the version is: `rambi_v1.6.68-a6608c8`:

*   board name = rambi
*   branch number = v1.6 (which is for the firmware-rambi branch)
*   number of commits on this branch (since the tag was added) = 68
*   latest git hash = a6608c8

The branch numbers (as of May 2014) are:

*   v1.0.0 cros/master
*   v1.1.0 cros/master
*   v1.2.0 cros/firmware-link-2695.2.B
*   v1.3.0 cros/firmware-snow-2695.90.B
*   v1.4.0 cros/firmware-skate-3824.129.B
*   v1.5.0 cros/firmware-4389.71.B
*   v1.6.0 cros/firmware-rambi-5216.B

Hack command to check the branch tags:

```
git tag

for hash in $(git for-each-ref --format='%(objectname)' refs/tags/); do
    git branch -a --contains $hash | head -1;
done
```

(If anyone can come up with something prettier, make a CL).

Run `util/getversion.sh` to see the current version string. The board name is
passed as an environment variable `BOARD`:

```bash
(chroot) $ BOARD="cheese" ./util/getversion.sh
```

```
cheese_v1.1.1755-4da9520
```
