# egpu-session-daemon

A systemd daemon that detects whether an NVIDIA eGPU is connected via Thunderbolt at boot, and automatically selects the appropriate desktop session before GDM/SDDM displays the login screen.

- **eGPU detected** &rarr; starts [Plasma](https://kde.org/plasma-desktop/)
- **iGPU only** &rarr; starts [GNOME](https://www.gnome.org/) (or whichever session you configure)

Written in C with atomic writes to the AccountsService file, because a simple bash script is not enough when power can go out mid-`sed`.

---

## Requirements

### Build Dependencies

| Distro | Required Packages |
|--------|-------------------|
| Fedora / RHEL | `gcc`, `systemd-devel`, `make` |
| Ubuntu / Debian | `gcc`, `libsystemd-dev`, `make` |
| Arch | `gcc`, `systemd-libs`, `make` (base-devel) |

Quick install:

```bash
# Fedora
sudo dnf install gcc systemd-devel make

# Ubuntu/Debian
sudo apt install gcc libsystemd-dev make

# Arch
sudo pacman -S gcc systemd make
```

### Runtime

- systemd (obviously)
- [AccountsService](https://www.freedesktop.org/wiki/Software/AccountsService/) — GDM, SDDM, and most display managers already use it.
- An NVIDIA eGPU connected via Thunderbolt (or an internal NVIDIA dGPU if you are not using Thunderbolt detection).

---

## Building and Installation

```bash
make
sudo make install
```

This installs the binary to `/usr/local/bin/egpu-session-daemon`.

### systemd Service

The `egpu-session-daemon@.service` file is a **parameterized service** (template). Replace `<username>` with your actual username:

```bash
sudo make install-service
sudo systemctl daemon-reload
sudo systemctl enable egpu-session-daemon@<username>.service
```

To test without rebooting:

```bash
sudo systemctl start egpu-session-daemon@<username>.service
journalctl -u egpu-session-daemon@<username>.service -n 20
```

---

## How It Works

1. **Waits for Thunderbolt to enumerate the eGPU** — polls for 10 seconds (`20 retries × 0.5s`).
2. **Reads sysfs directly** — scans `/sys/bus/pci/devices/` looking for vendor ID `0x10de` (NVIDIA) with a display controller class.
3. **Detects Thunderbolt** — verifies the GPU is behind an Intel PCI-to-PCI bridge (Thunderbolt controller), to distinguish the eGPU from an internal dGPU.
4. **Writes atomically** — creates a temporary file, syncs to disk with `fsync()`, and performs an atomic `rename()`. Even if power is lost mid-write, the file is never left in a corrupt state.
5. **Notifies systemd** — emits `READY=1` via `sd_notify()` for `Type=notify` compatibility.

### Why Not a Bash Script?

- **Atomic writes**: `sed -i` is not atomic. A power cut leaves the file truncated or empty. AccountsService breaks and fails to start the session.
- **Security**: C code allows strict username validation (prevents path traversal), symlink verification, and preservation of the original file's permissions/ownership.
- **Reliability**: No interpreter dependencies, pipes, or undefined behavior from text processing tools.

---

## Customization

Look for this line near the end of `egpu-session-daemon.c`:

```c
const char *session = has_egpu ? "plasma" : "gnome";
```

Replace `"plasma"` and `"gnome"` with the sessions you have installed. Common examples:

| Desktop | Session Name |
|---------|-------------|
| KDE Plasma | `plasma` |
| GNOME | `gnome` |
| GNOME (Wayland) | `gnome-wayland` |
| Sway | `sway` |
| i3 | `i3` |
| XFCE | `xfce` |

Then recompile and install:

```bash
make clean
make
sudo make install
```

---

## Security

- Strict POSIX username validation to prevent path traversal.
- Verifies the configuration file is a regular file (not a symlink) via `lstat()` + `S_ISREG()`.
- Temporary file created with `mkstemp()` to avoid race conditions.
- Preserves permissions and ownership of the original file via `fchmod()`/`fchown()`.
- File locking (`flock()`) to prevent race conditions between concurrent instances.
- Temporary file cleanup on signals (`SIGTERM`, `SIGINT`).

---

## License

Public Domain / Unlicense. Do whatever you want with this. If it saves your day, I'm glad.
