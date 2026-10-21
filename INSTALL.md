# OpenBSD Setup And Rollback

These instructions describe the tested OpenBSD 7.9 amd64 X61t configuration.
They are **not an automatic installer or a universal tablet recipe**.
Keep working SSH access and a way to reach the boot console before changing
kernel configuration. Save open work before any reboot.

## 1. Inspect Before Changing Anything

```sh
dmesg | grep '^com[0-9]'
ls -l /dev/cua00
printf 'find com\nexit\n' | doas config -e /bsd
```

The final command only inspects the kernel configuration and exits without
saving. The bridge accepts only the exact internal UART attachment described
in the README. Merely finding a `/dev/cua00` node does not prove attachment.

Do not probe other serial devices with this program. A different UART,
firmware state or hardware variant needs separate investigation.

## 2. Back Up The Existing Setup

Before editing, keep private copies of:

- `/bsd`, `/bsd.booted` and `/var/db/kernel.SHA256`.
- `/etc/bsd.re-config`, if it exists; otherwise record that it was absent.
- `/etc/X11/xenodm/GiveConsole` and `TakeConsole`.
- Your `.xsession`, plus an existing installed `x61-pen` binary.

Store administrator backups outside this repository with restrictive
permissions. Do not commit kernels, logs or private configuration files.

## 3. Persist The Internal UART Assignment

On the tested machine, a temporary boot-console `change com0` command was
lost on later boots. `/etc/bsd.re-config` lets OpenBSD's normal kernel
relinking reapply the change to the next kernel.

[examples/bsd.re-config](examples/bsd.re-config) is a **prompt-response
example**, not a shell script. It assumes that `find com` lists an enabled
ISA `com0` first and an ACPI `com0` second. It changes only the ISA port from
`0x3f8` to `0x200` and its IRQ from `4` to `5`, preserving the tested remaining
locators, and answers `n` for the ACPI entry.

**Check the prompts on your kernel. Do not apply the example unchanged if
the entries, their order, or their locator fields differ.** Preserve any
unrelated commands already in `/etc/bsd.re-config`.

Test the commands against a separate output kernel before installing them:

```sh
doas config -e -c ./examples/bsd.re-config -o /bsd.x61-preview /bsd
printf 'find com\nexit\n' | doas config -e /bsd.x61-preview
```

The expected ISA line contains `com0 at isa0 port 0x200` and `irq 5`; the
other device mappings should remain unchanged. The preview does not change
the running kernel. Keep or remove your preview as appropriate; it is not
automatically selected at boot.

After reviewing and installing/merging the rule into `/etc/bsd.re-config`,
use the native relink mechanism:

```sh
doas /usr/libexec/reorder_kernel
doas sha256 -C /var/db/kernel.SHA256 /bsd
printf 'find com\nexit\n' | doas config -e /bsd
```

Inspect `/usr/share/relink/kernel/GENERIC.MP/relink.log` if the relink fails.
Do not disable KARL, change `kern.allowkmem`, or replace a known-good kernel
to work around an unexplained error. Kernel-file edits alone cannot attach
the device to an already running kernel.

After a planned reboot, verify the actual attachment:

```sh
grep '^com0 at isa0 port 0x200/8 irq 5:' /var/run/dmesg.boot
```

Check the subsequent automatic relink as well, so the following boot will
retain the rule. The tested hardware reports an `ns16550a` with a 16-byte FIFO.

## 4. Grant Access Only To The Authenticated X Session

The normal serial-device owner is `root:dialer`, mode `0660`. Do not use
`chmod 666`, put the bridge in a root session, or loosen X authentication.

Review the fragments in `examples/`. Insert the GiveConsole fragment into
the **existing** `/etc/X11/xenodm/GiveConsole`, before its final `sessreg`
command, and the TakeConsole fragment into the existing `TakeConsole` at
the corresponding point. These files are executed by xenodm as part of
authenticated login/logout. **Do not replace either complete script with
a fragment.** Keep the files owned by root and non-writable by ordinary users.

The grant is conditional on the confirmed UART attachment. The authenticated
user gets mode `0600`; logout restores `root:dialer` and `0660`.

Check both complete scripts before ending the current session:

```sh
sh -n /etc/X11/xenodm/GiveConsole
sh -n /etc/X11/xenodm/TakeConsole
```

This is the tested xenodm path. A text-console `startx` login does not run
these hooks and needs a separately reviewed ownership policy; these examples
do not install or claim one.

## 5. Build And Start As The Desktop User

```sh
make
make check
make install
```

Back up an existing binary first. The default install prefix is
`$HOME/.local`; use `make PREFIX=/your/prefix install` if needed. No setuid
permissions are installed.

Insert one invocation before the final session-manager `exec` in your
existing `.xsession`, preserving the rest of the file:

```sh
"$HOME/.local/bin/x61-pen" &
```

After a normal graphical login, check `~/.xsession-errors`. Successful
initialization reports the baud rate, nonzero coordinate/pressure maxima,
panel dimensions, rotation and helper PID. Two `x61-pen` processes are normal.
There should be only one main bridge holding the serial device; the second
process is its cleanup helper.

Test pointer movement, tip clicks, the side button, and lifting the pen.
This does not test pressure-sensitive drawing: XTest does not expose a
tablet pressure axis to the application.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `Refusing port` | Actual current-boot attachment, not just the next kernel file. |
| `Device not configured` | UART attachment and resources. Device-node existence is insufficient. |
| Permission denied | Authenticated login hook, current owner and mode. |
| Serial setup fails / device busy | An existing bridge or another serial reader; do not run two copies. |
| No valid ISDV4 query | Baud, UART and device/protocol compatibility; do not remove the safety gate. |
| XTEST/RandR/LVDS-1 unavailable | The authorized display, output name, extensions and identity transform. |

Do not stop a working bridge merely to run tests. `make check` is isolated
from real hardware. Do not publish raw diagnostic recordings.

## Rollback

1. Remove the bridge invocation from your session startup and log out normally.
2. Restore the saved GiveConsole and TakeConsole scripts, then check them with
   `sh -n`. Stop any remaining **main** bridge with SIGTERM, not both processes
   with SIGKILL; allow the cleanup helper to finish.
3. Confirm that `/dev/cua00` has returned to its original `root:dialer`, `0660`
   policy. Restore the previous binary or remove only the one you installed.
4. Restore the previous `/etc/bsd.re-config`, or remove only these added
   commands if the file contains other rules. If the file was previously
   absent and contains only this rule, it can be removed.
5. With the installed kernel checksum valid, run the native relink again and
   verify the resulting next-boot mappings. Reboot only after saving work.

For boot trouble, use a known-good saved kernel from the local boot console.
Do not overwrite a later kernel with an old backup or replace the checksum
database blindly after updates. Inspect what changed and preserve recovery
access first.
