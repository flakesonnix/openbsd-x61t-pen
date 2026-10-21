# Synthetic Tests

Run `make check` on OpenBSD with its compiler and X sets installed.
Assertions must remain enabled; the integration harness rejects `NDEBUG`.

The normal executable's built-in self-tests cover coordinate/query decoding,
resynchronization, partial frames, touch rejection, and four rotations.

The integration harness includes the same source, renaming its entry point
and substituting only the hardware boundaries for the test build:

- `/var/run/dmesg.boot` becomes a synthetic successful UART attachment.
- `/dev/cua00` becomes a newly allocated pseudoterminal.
- Xvfb's output name and transform become an identity `LVDS-1` fixture.
- XTest, X authentication, X connections, process forking, pipes, polling,
  signals, serial termios and the cleanup helper remain real.

No substituted boundaries are compiled into the installed program.

`check.sh` chooses an unused display number, creates its own Xauthority file,
and starts a private Xvfb with TCP disabled. It never edits your Xauthority
file or connects the test to your desktop. Test recordings contain generated
bytes, not personal pen input.

The tests check deterministic stream recovery and then exercise the original
main loop with a generated range reply and a held tip/side button. They check
pointer mapping, button release and termios restoration on SIGTERM, SIGKILL
of the main process, and the stale-input watchdog.
The termios comparison excludes only `PENDIN`, a kernel-managed pending-input
state that can be set when restoring canonical mode; other flags, control
characters and both baud rates must match.

These tests do not certify real UART probing, firmware initialization, actual
login-hook execution, suspend/resume, physical panel rotation, or every
failure mode. Hardware results and synthetic results are deliberately
reported separately in the project README.
