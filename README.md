# Omapower

A per-user process watcher for power-profiles-daemon, with an optional Omarchy bar widget. It holds the performance profile while a whitelisted process is running and releases the hold afterward, restoring the profile selected underneath it (not necessarily balanced).

## Requirements

Linux `/proc`, systemd user services, power-profiles-daemon (`powerprofilesctl`), GLib/GIO and JSON-GLib, and Omarchy/Quickshell plus Font Awesome 7 Free Solid for the widget. Building also requires a C compiler, Linux headers, make and pkg-config. Kernel process events require `CONFIG_CONNECTOR` and `CONFIG_PROC_EVENTS`; availability and permissions are probed at startup, without requesting additional privileges.

## Configuration

By default Omapower watches `rustc` and `cargo` owned by the current user. To override the whitelist, create `~/.config/omapower/config.json`:

```json
{"commands": ["rustc", "cargo"]}
```

Names are exact Linux process `comm` names (at most 15 bytes), not paths or command-line arguments. Changes are picked up automatically. In the panel, use `+` to add a name or `×` to remove it; changes are saved to this file even if the service is stopped. The command-line equivalents are `omapower --add NAME` and `omapower --remove NAME`. Editing preserves other JSON fields and serializes Omapower's own writers, but does not lock out external editors. Invalid configuration stops the service and cannot be edited from the panel; correct it manually and check `journalctl --user -u omapower`.

## Event-driven operation

Omapower subscribes to Linux's proc connector (`NETLINK_CONNECTOR` / `CN_IDX_PROC`) before taking its initial `/proc` snapshot. Fork, exec, process-name and UID changes update individual processes through GLib's event loop. Matching processes are identified by PID and start time; pidfds, when available, also signal their exit. Thread names are not treated as process names, and an exited, unreaped process does not keep a hold. A zombie leader whose other threads are still executing remains active when pidfds are available.

Kernel notifications are best-effort: a process may finish before its name can be inspected or a hold can be acquired. Omapower rescans after detectable stream loss and once every 60 seconds to recover from silent drops. Configuration uses directory notifications, including atomic replacement, deletion/recreation and symlink-target updates; it is not reparsed on each process event. Status is written to `$XDG_RUNTIME_DIR/omapower.json` only when it changes.

If event subscription fails or is not acknowledged, Omapower logs the reason and falls back to scanning `/proc` every 500 ms. `omapower --poll` explicitly selects this fallback. The status file's `monitor` field reports `connecting`, `events` or `polling`, and the widget identifies polling in its service description. Older kernels and noninitial PID/user/network namespaces can restrict event access. Do not add root or capabilities just to avoid this fallback.

Builds started by another user or outside the service's visible PID namespace are not detected. Running Cargo for non-build commands also activates performance. A manual profile change that causes the daemon to release the hold is respected until no matching processes remain. Other applications' holds, thermal limits and system policy can affect the effective profile.

Power-profile calls are asynchronous. If power-profiles-daemon disappears, stale cookies are invalidated and an appropriate hold is acquired when it returns. Release failures retain the cookie and are retried; after three failed attempts, Omapower exits and closes its connection so the daemon drops the hold. An ambiguous acquisition failure or system-bus disconnection also causes an exit; the user service restarts it. Shutdown drops connection-owned holds, including pending acquisitions.

If explicit launch integration suits your workflow, no watcher is necessary:

```sh
powerprofilesctl launch --profile performance -- cargo build
```

This acquires the hold before the command starts, unlike reactive process detection.

## Local installation

Run `make` in this repository and keep it at a stable path. The widget requires the `plugin` directory to be linked from the config directory as described below, with the compiled `omapower` binary alongside it in the repository. Its persistent `omapower --watch-widget` helper observes systemd unit changes (`Subscribe` / `PropertiesChanged`) and configuration notifications independently of the automation service. Create `~/.config/systemd/user/omapower.service` with:

```ini
[Unit]
Description=Omapower process-based power profile holds
After=graphical-session.target

[Service]
ExecStart=/absolute/path/to/omapower/omapower
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
```

Run `systemctl --user daemon-reload` and `systemctl --user enable --now omapower`. Link `plugin` to `~/.config/omarchy/plugins/raw.omapower`, then use `omarchy plugin enable raw.omapower` to add the widget. The widget displays ⚡ for performance, ☯ for balanced, ♻ for power-saver, and `?` for an unknown profile. It reflects the actual system profile even if the Omapower service is stopped; hover for service details. Click it to view the configured whitelist and start or stop the user service. The toggle changes the running service only; it does not change whether systemd starts it at login. Service state and whitelist changes arrive through the persistent helper; the current power profile uses Quickshell's existing reactive `PowerProfiles` service. There are no periodic `systemctl`, shell whitelist or `busctl` queries. A one-shot retry restarts a failed helper after five seconds. The widget is optional: automation runs independently of the shell.

After rebuilding an existing installation, restart `omapower.service` and reload the shell/plugin to use the updated code. Stop with `systemctl --user disable --now omapower`; remove the plugin link and service file to uninstall. No system files or sudo are needed.

## Tests

`make test` runs the C configuration/process tests, command-line edit tests, JavaScript parser tests and isolated D-Bus integration tests. It requires `jq`, Node.js, `dbus-run-session`, and system Python with PyGObject (`python-gobject` on Arch; override `PYTHON` if necessary). When Quickshell is installed, it also runs a headless smoke test using the real widget/I/O components with visual-only Omarchy stubs.

Tests cover process identity and thread handling, subscription denial/timeouts, malformed packets, daemon replacement, pending calls, manual overrides, failed releases, zombie exit, configuration replacement/recreation/symlinks, stable idle status, fallback scanning, and systemd/widget notifications. All power-profile and systemd calls in integration tests go to private mock services, not the live system.
