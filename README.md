# Omarchy Display

An experimental, view-only TV receiver for an **independent Hyprland workspace**, with Omarchy bar controls. It creates a temporary virtual output on the computer and sends that output to a Samsung Tizen TV. The selected TV workspace stays visible while the laptop switches its own workspaces. This is extending the desktop, not mirroring the laptop panel.

**Verified on the UE40MU6120 at 720p:** a real moving workspace decoded about 24 frames/s in one measured run. The remaining mouse lag is noticeable; this is not a zero-latency monitor replacement. Higher FPS settings are caps, not performance guarantees. See the measurement scope below.

## Scope and status

- Send an existing numbered workspace to the TV, select it there, and keep computer focus on a physical monitor. Selecting another TV workspace changes what the TV displays; ordinary laptop workspace switching does not.
- A sent workspace moves to the virtual output; it is not rendered simultaneously on both outputs. Bring back returns a workspace to its recorded original monitor. Stop removes the owned virtual output and recovers moved content without closing applications.
- The receiver is **view-only, with no audio, keyboard/mouse forwarding or clipboard sharing**. Applications continue running on Linux. A matching TV-only interface cannot reproduce arbitrary Linux applications: that requires streaming their pixels or video.
- The implementation is a Python controller, C [wayvnc](https://github.com/any1/wayvnc)/[neatvnc](https://github.com/any1/neatvnc) capture over a private Unix socket, a Node.js bridge with an internal raw-RFB client and native [FFmpeg](https://ffmpeg.org/) JPEG encoder, and a handwritten ES5 TV receiver using native image decoding. It is not Rust, hardware H.264 streaming, a new desktop protocol, or a rewritten TV operating system.
- Real-device work is limited to **Samsung UE40MU6120, Tizen 3.0, firmware T-KTMDEUC 1410.1**. Pairing persistence, real workspace rendering, bar-panel Start, workspace transfer/recall and independent computer-workspace switching have been exercised. Other Samsung models, firmware versions and TV platforms remain unverified.

For media playback rather than a Linux desktop, see the separate [Jellyfin TV integration](integrations/jellyfin-tv/README.md).

## Easier alternatives

A cable is the simplest reliable independent monitor **with audio**: use the computer's HDMI output, or a suitable USB-C-to-HDMI/DisplayPort adapter **only if that USB-C port supports video output**. [VESA's DisplayPort over USB-C overview](https://www.displayport.org/displayport-over-usb-c/) explains the Alternate Mode requirement. Configure the TV as an extended display, not a mirror.

[GNOME Network Displays](https://github.com/GNOME/gnome-network-displays) is an experimental Miracast implementation. It depends on compatible Wi-Fi Direct hardware/drivers, NetworkManager with `wpa_supplicant` rather than `iwd`, an appropriate screencast portal, and video/audio encoders. It has **not been tested on this setup**, and streaming a selected monitor does not by itself provide this project's selected virtual-workspace behavior. No built-in Chromecast or AirPlay support is assumed for the 2017 TV.

## Requirements

### Linux host

- Linux x86-64 in the target Hyprland session, with `hyprctl`, Wayland and a private `XDG_RUNTIME_DIR`. The controller uses Hyprland's Lua `hyprctl eval` interface; older configurations/APIs are not interchangeable.
- A current Omarchy shell with its Quickshell plugin SDK for the native bar popup. Without `omarchy-shell`, the installer installs the host CLI without registering the bar or shortcuts.
- Python **3.10+** with `venv` and `ensurepip`; Node.js **20+**, npm, FFmpeg (`ffmpeg`, with JPEG encoding support), Git, a C compiler (`cc`), `pkg-config`, and `wayland-scanner`.
- Native development libraries discoverable through `pkg-config`: `wayland-client`, `wayland-protocols`, `pixman-1`, `libdrm`, `xkbcommon` (1.0.0+), `jansson`, `zlib`, and `libturbojpeg`. Install the corresponding packages for your distribution yourself.
- Network access during installation to fetch pinned npm dependencies, upstream capture sources and hash-pinned Meson/Ninja tools. Build tools and capture sources stay in the application's user cache; the system's wayvnc is not replaced.

### TV and network

A Samsung TV that permits owner-installed Tizen web applications, remote control, Developer Mode, and an owner signing profile accepted by that TV. The host and TV need reachable private IPv4 addresses on a trusted, isolated LAN. The host is restricted to the configured interface and exact TV peer address; allow the chosen TCP port in your local firewall only as narrowly as needed. The default is `6080`. Address changes require updating the private configuration.

The TV SDK/signing tools are separate from the Linux host installer. No rooting, firmware modification, trust-store changes, Home-button replacement or autostart is required or installed.

## Install and update the host

From this source checkout, as your desktop user, **not with sudo**:

```sh
./install.sh
omarchy-display init
omarchy-display size 720p --fps 30  # Recommended starting point for the tested older TV
omarchy-display start
```

Ensure `~/.local/bin` is on your `PATH`. `init` asks for the host's private interface address and the authorized TV address, then generates a private pairing secret. It refuses to overwrite existing configuration.

The installer builds the pinned, output-locked capture binary, installs the host and static TV receiver source, and registers the native bar widget and conflict-checked shortcuts when Omarchy shell is available. The CLI is installed under `~/.local/bin`. It does not install OS packages, open firewall ports or start sharing automatically. It refuses to overwrite unowned or locally changed installed files.

To update, obtain the desired source revision and run `./install.sh` again. An update stops an installed display session; start it explicitly afterwards. Pairing configuration is retained.

## Install the TV receiver

**Manual signing is a prerequisite, not a universal one-click step.** Follow Samsung's [TV SDK setup](https://developer.samsung.com/smarttv/develop/getting-started/setting-up-sdk/installing-tv-sdk.html) and [certificate instructions](https://developer.samsung.com/smarttv/develop/getting-started/setting-up-sdk/creating-certificates.html). You need your own author/distributor certificate profile authorized for your TV, plus the SDK's `tizen` and `sdb` tools and Developer Mode access.

For Tizen 3.0 in particular, do not assume a profile generated by today's tooling will be accepted by an older TV. Certificate compatibility, SDK extensions and signing services are external prerequisites. The helper cannot obtain a legacy-compatible certificate, bypass signature checks or fix certificate rejection. Keep the author certificate for future updates; changing author identity can prevent an in-place update.

The receiver is static HTML/CSS and handwritten ES5 JavaScript; it does not require a noVNC bundle or an npm receiver-build step before packaging.

The root `tv.py` helper packages public receiver source, delegates signing to an explicitly selected SDK profile, and exposes separate install/launch operations. It does not create certificates or discover passwords. Replace the SDK paths, profile and TV address below with your own:

```sh
TIZEN=/path/to/tizen-studio/tools/ide/bin/tizen
SDB=/path/to/tizen-studio/tools/sdb
PROFILE=YOUR_PROFILE
TV_ADDRESS=YOUR_TV_PRIVATE_IP

python3 tv.py package --app apps/desktop-tv --output dist/omarchy-display-unsigned.wgt
python3 tv.py sign --input dist/omarchy-display-unsigned.wgt \
  --output dist/omarchy-display-signed.wgt --tizen "$TIZEN" --profile "$PROFILE"
python3 tv.py install --widget dist/omarchy-display-signed.wgt \
  --target "$TV_ADDRESS" --tizen "$TIZEN" --sdb "$SDB"
python3 tv.py launch --application-id OmarchyTV1.Display \
  --target "$TV_ADDRESS" --sdb "$SDB"
```

The public receiver identity is `OmarchyTV1.Display` / package `OmarchyTV1`. For a separate identity, packaging accepts `--application-id` and `--package-id` together; launch the same application ID you packaged. Keep the identity and author certificate stable for updates. See `python3 tv.py --help` and subcommand help. An unsigned widget is not installable, and a successful SDK command alone does not demonstrate correct rendering or performance on a physical TV.

Never publish owner-signed widgets, certificate files, signing profiles, account details or device identifiers. No owner credentials or signed packages are supplied here.

## Pair and use

On the host, display the private connection details only when needed:

```sh
omarchy-display pair
```

Open the installed receiver on the TV, enter the endpoint and **full 32-hex-character pairing secret**, and connect. This is a random **128-bit authentication credential**, not a six-digit code, a TV signing certificate or a Samsung account password.

The receiver remembers the endpoint and secret **only after the first desktop image decodes successfully**, in that app's `localStorage`. Normally you enter them once; it reconnects on subsequent launches and when returning to the foreground. Its fit preference is also local. If TV storage fails, it reports session-only pairing. `localStorage` is not an encrypted vault. Remote Back disconnects; returning to the foreground can reconnect using saved pairing. Disconnecting the TV does not stop the host output.

### Controls

Left-click the Omarchy monitor icon for the native popup: Start, Stop, numbered workspace Send/Bring back, and capture size/FPS for the next start. Pairing opens a private terminal only after an explicit click; pairing details never appear in status or the bar.

The icon remains visible when stopped. A hollow accent dot means ready with no viewer; a filled dot means the host reports an authenticated capture viewer, **not measured playback quality**. Error and starting states are distinct. Status is polled, not instantaneous. See [bar widget details](omarchy-plugin/README.md).

When registered successfully, `SUPER+CTRL+ALT+1` through `9` send/select that numbered workspace on the TV; `SUPER+CTRL+ALT+0` means workspace **10**. The installer checks for conflicts rather than overwriting existing shortcuts. The selected workspace must already exist, and the host display must be started.

```sh
omarchy-display workspace 3       # Send/select existing workspace 3 on the TV
omarchy-display recall 3          # Return it to its recorded original monitor
omarchy-display status            # Safe summary; no pairing credential
omarchy-display status --json
omarchy-display menu              # Interactive terminal alternative
omarchy-display stop
```

`omarchy-display move WINDOW_ADDRESS` moves one window, using a real address from `hyprctl clients -j`; the terminal menu can move the previously focused window without copying an address. Bring back is available only for workspaces whose original monitor was recorded in this session, not the virtual output's automatically created workspace. Commands do not promise duplicated rendering.
In the receiver, **Fit: Screen** scales the workspace to the app viewport; **Fit: App pixels** keeps source pixels at app-pixel size, with borders or clipping as necessary. This does not bypass Samsung's application-resolution scaling to the physical panel.

### Dimensions and performance

```sh
omarchy-display size 720p --fps 15
omarchy-display size 1080p --fps 30
omarchy-display detect                 # Refresh the TV's reported resolution
omarchy-display size native --fps 30
```

`720p` is 1280×720; `1080p` is 1920×1080. FPS choices are 15, 30 and 60, and are **capture caps, not achieved frame rates**. Defaults are 1080p/30. Size/FPS changes are saved for the **next start only**: Stop and Start explicitly to apply them; saving never silently restarts a session.

Capture dimensions, the receiver's browser viewport and a TV-reported panel resolution are different facts. On the tested TV, the app and Tizen `SystemInfo DISPLAY` report 1920×1080, while the read-only Samsung device API (`http://TV_ADDRESS:8001/api/v2`, `device.resolution`) reports 3840×2160. The latter is an API report, not a physical measurement; its source must remain visible rather than presenting the browser viewport as native-panel proof. `native` requires supported, explicitly reported dimensions; without them it is unavailable. Selecting a reported 4K size does not establish usable 4K performance.

`init` and `start` also probe the configured TV's read-only device API; `status` and `size` never make network probes. Detection caches only dimensions and their source in private state. A failed probe retains an earlier verified report, if present; without one native size stays unknown. Status exposes the report as `panel` with source `samsung-device-api`. Supported native capture is capped at the area of 3840×2160.

The bridge sends complete JPEG images over WebSocket with at most one outstanding image, then waits for the receiver's display acknowledgement. It retains the latest frame rather than building a FIFO of stale images. This bounds queued work; it does not guarantee capture-to-screen latency. Full-frame JPEG trades bandwidth and encoding/decoding work for a simpler old-TV receiver. There is no audio.

Measured using a real terminal workspace changing at 30 Hz on this TV, with the 720p/30 preset:

| Measurement | Observed result |
| --- | --- |
| Decoded desktop images | 241 in 10.02 seconds, about 24 frames/s |
| Encoding-complete to TV DOM image swap | Mean 42.5 ms; 202 samples |
| Observed delay range in that sample | 20–121 ms |
| Clock-offset probe round trip | 8 ms |

These are separate short runs, not a guaranteed minimum frame rate. The delay measurement **excludes desktop capture, encoding and physical panel scan-out**; it is not glass-to-glass latency. The owner also reported a substantial improvement in screensaver refresh and mouse tracking, with residual mouse lag. Lower dimensions reduce bandwidth and work; native/4K performance has no equivalent claim. H.264 and VP8 MediaSource probes did not play successfully on this device despite advertised support, so they are not shipped as a hidden fallback.

## Security and revocation

**Plain `ws://` is unencrypted: use a trusted, isolated LAN only.** Exact-peer restriction and the random secret authenticate access but do not prevent LAN interception or man-in-the-middle attacks. Do not forward the port to the Internet. Optional `wss://` requires a certificate already trusted by the TV; the private host configuration can specify `tls.key` and `tls.cert` paths. TLS provisioning/old-TV compatibility is not automated, and this project does not change the TV trust store.

Host configuration is stored at `$XDG_CONFIG_HOME/omarchy-display/config.json` (default `~/.config/omarchy-display/config.json`), with private file permissions. Runtime state/logs and installer ownership records use `$XDG_STATE_HOME/omarchy-display`; build cache uses `$XDG_CACHE_HOME/omarchy-display`; installed code uses `$XDG_DATA_HOME/omarchy-display` (defaults `~/.local/state`, `~/.cache`, and `~/.local/share`). Private sockets live under `XDG_RUNTIME_DIR`.

To revoke a TV, use **Forget pairing** in the receiver to clear its local saved credential, then rotate the host secret to invalidate any retained copies:

```sh
omarchy-display stop
omarchy-display rotate
omarchy-display start
omarchy-display pair
```

Re-enter the new secret only on TVs you still trust. Forget pairing alone does not revoke a copied credential; rotation does. Keep pairing details out of Git, screenshots, issue reports, shell transcripts and logs. `pair` deliberately prints a secret: run it locally, not into captured output.

## Uninstall

From the checkout:

```sh
./install.sh --uninstall                  # Retain private pairing configuration
./install.sh --uninstall --purge-config   # Also remove host pairing configuration
```

The installed equivalent is `omarchy-display uninstall` (optionally `--purge-config`). Uninstall stops the owned session and removes owned launcher, plugin, shell registration and shortcuts while preserving unrelated/changed files. Build cache, private logs and shell backups are retained. Purging the host configuration does not erase TV `localStorage`; use Forget pairing on the TV. Remove the TV widget through the TV's app management separately.

## Source and attribution

The host installer is also the source-build entry point. For a standalone capture build, `python3 desktop/build-wayvnc.py` uses the pinned sources/tools and native dependencies above, writes to the application XDG cache, and does not install or start the server. `npm ci --prefix desktop` installs the bridge's locked dependencies; the TV receiver needs no JavaScript bundle build. Generated files and packages are not source.

The project began from [CleanFrame](https://github.com/nthnclrk/CleanFrame/tree/fa537d1892b71d2de2bf94c24cbff3812e9f70d5), commit `fa537d1892b71d2de2bf94c24cbff3812e9f70d5`, by Nathan Clark (2026), under the [MIT license](LICENSE). CleanFrame provided a remote-oriented Samsung home-screen launcher, app launching, layouts/settings, wallpaper and clock UI, ES5 runtime conventions and TV setup groundwork. It did **not** provide this independent Linux workspace extender or the Jellyfin compatibility work. Its reference TV differs from this project's tested device; its compatibility and native Home/startup behavior are not inherited promises here.

The extender adds host lifecycle/workspace controls, an output-lock integration around existing wayvnc/neatvnc, the authenticated asynchronous bridge and bounded JPEG-frame delivery, the ES5 receiver, pairing persistence, and Omarchy controls. Existing capture protocols and JPEG encoding are not original inventions of this project.

The [Jellyfin integration](integrations/jellyfin-tv/README.md) is the full existing **Jellyfin Web 12.2** application plus the official [Jellyfin Tizen wrapper](https://github.com/jellyfin/jellyfin-tizen), adapted for the older engine—not a media client authored from scratch. Its build requirements and compatibility limits are separate from the desktop receiver's.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for component provenance, licenses and modifications. Third-party code retains its own license; the root MIT license does not relicense it. Original commercial-channel artwork, private development history, owner certificates and signed widgets are not part of the public source. This is an independent project, not affiliated with Samsung, Omarchy or Jellyfin.
