<div align="center">

# Omapower

<img src="assets/bolt.svg" width="28" alt="Performance"> &nbsp;&nbsp; <img src="assets/yin-yang.svg" width="28" alt="Balanced"> &nbsp;&nbsp; <img src="assets/recycle.svg" width="28" alt="Power saver">

**Performance when you need it. Your usual profile when you don't.**

</div>

Omapower watches your processes and holds the performance profile while a configured command is running. An optional Omarchy bar widget shows the current profile and lets you manage the service and command list.

## Get started

You'll need Linux, power-profiles-daemon, a system D-Bus, GLib/GIO and JSON-GLib. To build, install a C compiler, Linux headers, make and pkg-config. The optional widget also needs Omarchy/Quickshell and Font Awesome 7 Free Solid.

Build with `make` and keep the repository at a stable path. Create `~/.config/systemd/user/omapower.service`, replacing the path with the absolute path to your built binary:

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

```sh
systemctl --user daemon-reload
systemctl --user enable --now omapower
```

For the optional widget, link this repository's `plugin` directory to `~/.config/omarchy/plugins/raw.omapower` (or under `$XDG_CONFIG_HOME` if set), then run `omarchy plugin enable raw.omapower`. Click the widget to manage commands or start and stop the service. The toggle doesn't change whether the service starts at login.

After rebuilding, restart the service and reload the shell/plugin. To uninstall, run `systemctl --user disable --now omapower` and remove the service file and plugin link.

## Choose commands

Omapower watches `rustc` and `cargo` by default. Add or remove names in the widget, or use:

```sh
omapower --add NAME
omapower --remove NAME
```

Names must match Linux process `comm` names exactly: 1–15 bytes, without slashes or newlines. They are not paths or command-line arguments. The list lives in `$XDG_CONFIG_HOME/omapower/config.json` (normally `~/.config/omapower/config.json`):

```json
{"commands": ["rustc", "cargo"]}
```

Changes are picked up automatically. An invalid file stops the service and prevents widget edits; fix it manually and check `journalctl --user -u omapower`. Omapower serializes its own edits, but external editors aren't locked out.

## How it works

Omapower watches processes owned by your user in its visible PID namespace. It uses kernel process events when available, falling back to a 500 ms scan when they aren't; `omapower --poll` forces scanning. Event detection is best-effort, so very short-lived processes may be missed. You don't need root or extra capabilities.

A manual profile change that releases Omapower's hold is respected until the matching processes finish. Other applications' holds and system policy can also affect the effective profile. The service handles daemon restarts and retries failed releases; systemd restarts it after an unrecoverable error.

For a command you launch yourself, `powerprofilesctl` offers a simpler alternative that acquires the hold before the command starts:

```sh
powerprofilesctl launch --profile performance -- cargo build
```

## Tests

Run `make test` for the C, CLI, JavaScript and isolated D-Bus integration tests. These require `jq`, Node.js, `dbus-run-session` and Python with PyGObject (`python-gobject` on Arch; override `PYTHON` if needed). The tests use mock power-profile and systemd services; check the widget in Omarchy separately.

Header icons by [Font Awesome](https://fontawesome.com), licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
