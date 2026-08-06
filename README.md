# Succubid

Synchronizes MPV video playback with The Handy funscript playback.

Configuration can be provided through command-line options or environment variables. Command-line options take precedence.

For safety, `succubid` refuses to run as root.

---

# Options

CLI flags and their corresponding environment variables.

| Flag | Long | Environment Variable | Description | Default |
|---|---|---|---|---|
| `-k` | `--key` | `SUCCUBID_HANDY_CONNECTION_KEY` | Handy connection key **(required)** | |
| `-f` | `--firmware` | `SUCCUBID_HANDY_API_VERSION` | Handy firmware version (`FW3` or `FW4`) | `FW3` |
| `-a` | `--auth` | `SUCCUBID_HANDY_CONNECTION_AUTH` | Handy V3 API authentication key. Required when using `FW4`. Ignored on `FW3`. | |
| `-s` | `--socket` | `SUCCUBID_MPV_SOCKET_PATH` | Path to the MPV IPC socket | `/tmp/mpv.sock` |
| `-u` | `--upload` | `SUCCUBID_HANDY_UPLOAD_SERVER` | Script upload server URL | `https://www.handyfeeling.com/api/hosting/v2/upload` |
| `-l` | `--local` | `SUCCUBID_SERVE_LOCAL` | Serve scripts locally instead of uploading. Supported on `FW3` only. | `false` |
| `-g` | `--gui` | `SUCCUBID_USE_GUI` | Prompt for a script inside MPV when multiple matching funscripts are found | `false` |
| `-h` | `--help` | | Display help message and exit | |

## Environment Variable Values

For boolean options (`SUCCUBID_SERVE_LOCAL` and `SUCCUBID_USE_GUI`), accepted true values (case-insensitive) are:

- `true`
- `yes`
- `on`
- `1`

Any other value is treated as `false`.

---

# Examples

Minimum required configuration:

```bash
succubid -k CONNECTION_KEY
```

Use a FW4 device:

```bash
succubid -k CONNECTION_KEY -f FW4 -a AUTH_KEY
```

Serve scripts locally:

```bash
succubid -k CONNECTION_KEY -l
```

Enable the script selector:

```bash
succubid -k CONNECTION_KEY -g
```

Use a custom MPV socket:

```bash
succubid -k CONNECTION_KEY -s /run/user/1000/mpv.sock
```

Configure with environment variables:

```bash
export SUCCUBID_HANDY_CONNECTION_KEY=CONNECTION_KEY
export SUCCUBID_USE_GUI=true
succubid
```

---

# Exit Status

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Failure |

---

# Building

## Prerequisites

- `g++` (with c++20 support)
- `curl` / `libcurl`
- `xxd`
- `cpp-httplib`
- `nlohmann_json`

by default the makefile assumes these are either installed system-wide, or in the case of macOS, via homebrew

## Compile

```bash
make BUILD=release
```

## OS Specific

### Arch
```bash
makepkg -si
```

### MacOS
```bash
brew install gcc make cpp-httplib nlohmann-json mpv && make
```

### FreeBSD
```bash
pkg install curl mpv xxd nlohmann-json cpp-httplib gcc gmake && gmake
```

### Fedora
```bash
dnf install curl mpv xxd libcurl-devel json-devel cpp-httplib-devel gcc gcc-c++ make && make
```

### Linux/Macos w/ nix
```bash
nix profile install github:UnknownPleasuresDev/succubid
```

# Nix modules

## Home-manager
The home-manager module will automatically configure your environment and set you up with packages you can use
- MPV configuration to the mpv socket
- Environment variables for connection key, mpv socket connection and whether gui is enabled
- Provides packges:
  - succubid (wrapped with configured arguments)
  - unwrapped-succubid (just the succubid binary)
  - succubid-gui (a terminal to keep open that runs succubid in a window, useful if you don't want to enable the service)
  - succubid-gui-unwrapped (the succubid-gui but with no wrapper args, if you want to override the configuration)
  - install-succubid (A shell script to install the user-level systemd units to configure succubid
  - succubid-mpv (a wrapped version of mpv that has succubi args)

## Home-manager options

### Service
This will enable the background process to run 24/7
```
services.succubid.enable = true;
```

### connectionKey
This is where you handy code goes
```
services.succubid.connectionKey = "abcfdg";
```

### Gui
This will enable the funscript selector in MPV when there are multiple versions of funscript detected
```
services.succubid.gui = true;
```

### mpvSocket
This will customize the MPV Socket location, most of the time you shouldn't need to change this
```
services.succubid.mpvSocket = "/path/to/socket/location";
```

# Disclaimer

Succubid is not affiliated with, endorsed by, or sponsored by Ohdoki AS or The Handy.
