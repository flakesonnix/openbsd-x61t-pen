# Security And Privacy

This is experimental local input software, not a privileged system service.
Use it only as the authenticated desktop user with access to the confirmed
internal serial port and that user's authorized X display.

The bridge can inject pointer movement and clicks. Incorrect input or a
defect can therefore affect whatever is visible in the session. Keep the
serial-device gate, exclusive access, restrictive permissions and X
authentication intact. Do not install setuid or run the bridge as root.

The helper reduces stuck-button and terminal-state failures when the main
process dies. It cannot guarantee cleanup if both processes are killed,
the system fails, or hardware becomes unresponsive. Synthetic tests cover
specific cases, not every possible failure.

Verbose output and raw captures may reveal handwriting or interaction
patterns. Share only minimal synthetic/anonymized reproductions. Never post
private recordings, core dumps, full system logs, or kernel images.

For a vulnerability, use GitHub's private reporting mechanism if available.
Otherwise open a high-level issue asking for a private contact method without
posting sensitive data or exploit details. Do not assume a public issue is a
private reporting channel.
