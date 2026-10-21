# openbsd-x61t-pen

An experimental serial ISDV4-to-XTest pointer bridge for the ThinkPad X61
Tablet on OpenBSD. Small C program, base-system X11 libraries, no Linux input
API and no desktop framework.

**This is a mouse-pointer bridge, not a native Wacom/XInput tablet driver.**
It reads pressure but does not deliver pressure, tilt, or a separate tablet
device to drawing applications. Do not expect pressure-sensitive strokes.

## Tested Hardware

- ThinkPad X61 Tablet, OpenBSD 7.9 amd64, Xenocara and IceWM.
- Internal serial digitizer at ISA port `0x200`, IRQ `5`, exposed as
  `/dev/cua00` at 19200 baud.
- Internal `LVDS-1` panel at 1024x768 with an identity RandR transform.
- A complete system reboot, graphical-login autostart, valid ISDV4 range
  query and basic physical pointer operation have been verified.

This is one tested machine, not a claim of support for all X61t variants,
other tablets or other OpenBSD releases. Suspend/resume, physical rotation,
eraser behavior and extended drawing sessions need further hardware testing.
The coordinate mapping has synthetic tests for all four rotations.

## What It Does

- Maps pen proximity coordinates to the internal display.
- Maps the pen tip to mouse button 1 and the side button to button 3.
- Ignores eraser clicks; it does not create a drawing-application eraser tool.
- Rejects touch packets and coordinates outside the queried pen range.
- Releases buttons after stale input and on orderly shutdown.
- Uses a cleanup helper to retire the original X client, release buttons,
  stop sampling and restore termios when the main process exits or is killed.
- Uses exclusive serial access and OpenBSD `pledge(2)`.

It does not discover arbitrary serial ports. Startup requires a matching
`com0 at isa0 port 0x200/8 irq 5:` line in `/var/run/dmesg.boot`, an authorized
X display, XTEST, RandR, and the expected panel. Unsupported configurations
are refused rather than guessed. The helper is a second `x61-pen` process;
that is expected, not duplicate autostart.

## Build And Test

Install OpenBSD's compiler and X sets. No third-party packages are required.

```sh
make
make check
```

`make check` runs parser/mapping self-tests and synthetic X11/serial tests.
It creates a private, authenticated Xvfb display and a pseudoterminal.
It **does not open the real digitizer, change its permissions, or use your
desktop display**. Tests cover parser recovery, ordinary termination,
SIGKILL of the main process, button release, and serial-setting restoration.
See [tests/README.md](tests/README.md) for the test boundaries.

## Install And Run

Read [INSTALL.md](INSTALL.md) first. It covers backups, the hardware-specific
UART workaround, xenodm permissions, autostart, and rollback.

The default binary location is `$HOME/.local/bin`. Back up an existing binary
before replacing it, and do not replace a running installation just to test
a build.

```sh
make install
"$HOME/.local/bin/x61-pen"
```

Run the bridge as your logged-in desktop user, **not as root**. Kernel setup
and login-hook edits are separate administrator operations; `make install`
does not perform them. There is no privileged installation script.

### Options

| Invocation | Purpose |
| --- | --- |
| `x61-pen` | Run the foreground bridge on the authorized X display. |
| `x61-pen -v` | Also log raw pen coordinates, pressure and button states. |
| `x61-pen --self-test` | Run built-in synthetic tests; no tablet or X server needed. |
| `x61-pen --decode FILE` | Count pen/tip frames in a raw recording without opening the tablet. |

Verbose logs and recordings can reveal handwriting and interaction patterns.
Do not post personal captures, full system logs, core dumps or kernel images.

## Limitations And Safety

This early experimental utility is specific to the tested hardware. It is not
an OpenBSD kernel driver or an official OpenBSD, Lenovo or Wacom component.
Changing a serial-port mapping on unrelated hardware can interfere with
other devices. Never make serial devices world-writable to get it running.

The cleanup helper is not protection against every failure: killing both
processes, a system crash, or broken hardware can prevent cleanup. The
synthetic tests do not certify all failure paths, cold boots, real firmware
behavior, resume, or every panel configuration. See [SECURITY.md](SECURITY.md).

## License

**GPL-2.0-or-later**, with the existing Linux Wacom / Red Hat attribution
preserved. See [LICENSE](LICENSE) and [NOTICE](NOTICE). This publication does
not replace that license with ISC.
