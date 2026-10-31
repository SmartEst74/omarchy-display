# Omarchy Display

An experimental, view-only Samsung TV display for a **selected Hyprland workspace**, with Omarchy bar controls. **Mirror** keeps the same live workspace on the computer and TV, even while the computer switches to other workspaces. **Extend** remains a separate option that transfers a workspace to an independent virtual output.

**GPU-differenced desktop updates:** DMA-BUF capture, exact changed-tile detection, uniform-colour classification and JPEG-atlas packing run on the GPU. Uniform changes become exact RGBA fills on the TV GPU; TurboJPEG compresses changed non-uniform content on the CPU. Atlas batching reduces expensive TV native-image uploads without changing JPEG quality. There is no buffered video player. Native 3840×2160 capture is retained, but the tested TV's 1920×1080 app graphics surface does not establish true 4K panel detail. FPS settings are caps, not performance guarantees.

## Scope and status

- **Mirror (fresh-install default):** select an existing numbered workspace. Windows stay on their original computer monitor; the TV keeps rendering that workspace when the computer switches away. Mirror creates **no additional desktop output**, so the pointer cannot extend onto the TV. The captured cursor appears only while the pointer is on the selected source with that workspace visible there; switching away clears it. This is neither a frozen snapshot nor whole-monitor mirroring that follows the active workspace.
- **Extend:** move a selected workspace to the temporary TV output. Bring back returns it to its recorded original monitor. Stop removes the owned output and recovers transferred content without closing applications. Existing v1.1 settings retain Extend until explicitly changed.
- The receiver is **view-only, with no audio, keyboard/mouse forwarding or clipboard sharing**. Applications continue running on Linux. A matching TV-only interface cannot reproduce arbitrary Linux applications: that requires streaming their pixels or video.
- Mirror uses an exact-version C++ Hyprland plugin, reused private render resources and GPU letterboxing. It exports offscreen DMA-BUF frames and native GPU fences directly to `mirror-capture`; no Wayland monitor or headless sink is created. Extend retains output-locked C [wayvnc](https://github.com/any1/wayvnc) capture. Both use pinned [neatvnc](https://github.com/any1/neatvnc), a GLES 3.1 changed-region encoder, an authenticated Node.js bridge and an ES5 WebGL receiver. The CPU does not scan frames for differences or copy pixels to pack atlases. Photos and other non-uniform changes still require CPU JPEG compression.
- Real-device work is limited to **Samsung UE40MU6120, Tizen 3.0, firmware T-KTMDEUC 1410.1**. Pairing reuse, actual TV decoding, simultaneous workspace mirroring, live inactive-workspace updates, unchanged source-window ownership, and Extend transfer/recall have been exercised. Other Samsung models, firmware versions and TV platforms remain unverified.

For media playback rather than a Linux desktop, see the separate [Jellyfin TV integration](integrations/jellyfin-tv/README.md).

## Easier alternatives

A cable is the simplest reliable independent monitor **with audio**: use the computer's HDMI output, or a suitable USB-C-to-HDMI/DisplayPort adapter **only if that USB-C port supports video output**. [VESA's DisplayPort over USB-C overview](https://www.displayport.org/displayport-over-usb-c/) explains the Alternate Mode requirement. Configure the TV as an extended display, not a mirror.

[GNOME Network Displays](https://github.com/GNOME/gnome-network-displays) is an experimental Miracast implementation. It depends on compatible Wi-Fi Direct hardware/drivers, NetworkManager with `wpa_supplicant` rather than `iwd`, an appropriate screencast portal, and video/audio encoders. It has **not been tested on this setup**, and streaming a selected monitor does not by itself provide this project's selected virtual-workspace behavior. No built-in Chromecast or AirPlay support is assumed for the 2017 TV.

## Requirements

### Linux host

- Linux x86-64, **Hyprland 0.56.2 at commit `efb50993780079460b0cbed1363e2166a2de1d9f`**, matching development headers, `hyprctl`, Wayland, the OpenGL renderer, and a private `XDG_RUNTIME_DIR`. The controller uses Hyprland's Lua `hyprctl eval` interface.
- A current Omarchy shell with its Quickshell plugin SDK for the native bar popup. Without `omarchy-shell`, the installer installs the host CLI without registering the bar or shortcuts.
- Python **3.10+** with `venv` and `ensurepip`; Node.js **20+**, npm, Git, C and C++23 compilers compatible with the installed Hyprland headers, `pkg-config`, and `wayland-scanner`.
- Native development libraries discoverable through `pkg-config`: `hyprland`, `wayland-client`, `wayland-server`, `wayland-protocols`, `pixman-1`, `libdrm`, `libinput`, `libudev`, `pangocairo`, `egl`, `glesv2`, `xkbcommon` (1.0.0+), `jansson`, `zlib`, `gbm`, `libturbojpeg`, and `lua5.4` or `lua`. Install the corresponding OS packages yourself. FFmpeg and VAAPI encoding libraries are not desktop-stream dependencies.
- Installation fetches pinned npm dependencies, capture sources, and hash-pinned Meson/Ninja/CMake wheels. Tools and capture sources stay in the application's private user cache; no global CMake installation or replacement system wayvnc is needed.
- A GBM/EGL driver supporting DMA-BUF import/export, native GPU fences, renderable linear GBM buffers, surfaceless contexts and hardware GLES **3.1 compute shaders**. The verified host is Intel Broadwell HD Graphics 6000 with Linux `i915` and Mesa. The render node is resolved from the compositor's actual EGL display, not an optional GBM descriptor or a guessed primary GPU. Software GL renderers are refused; GPU initialization, packing or encoding failure stops the stream instead of falling back to CPU comparison/packing. Other GPUs and HDR source displays remain unverified.

**Compositor risk:** Mirror hooks internal rendering functions and can crash the desktop. Save work before enabling it. Build-time commit checks and runtime commit/API-hash checks refuse mismatched versions; a Hyprland upgrade needs a reviewed port and fresh isolated testing, not disabling these checks. The plugin is loaded only for Mirror and unloaded on Stop; it is not added to your Hyprland configuration. Mirror stops and disconnects the receiver when the computer session locks; unlock and explicitly start it again.

### TV and network

A Samsung TV that permits owner-installed Tizen web applications, remote control, Developer Mode, and an owner signing profile accepted by that TV. The host and TV need reachable private IPv4 addresses on a trusted, isolated LAN. The host is restricted to the configured interface and exact TV peer address; allow the chosen TCP port in your local firewall only as narrowly as needed. The default is `6080`. Address changes require updating the private configuration.

The TV SDK/signing tools are separate from the Linux host installer. No rooting, firmware modification, trust-store changes, Home-button replacement or autostart is required or installed.

## Install and update the host

From this source checkout, as your desktop user, **not with sudo**:

```sh
./install.sh
omarchy-display init
omarchy-display size native --fps 30  # Requires a supported TV-reported native size
omarchy-display start
```

Ensure `~/.local/bin` is on your `PATH`. `init` asks for the host's private interface address and the authorized TV address, then generates a private pairing secret. It refuses to overwrite existing configuration.

The installer builds the pinned output-locked capture binary and version-matched compositor plugin, installs the host and static TV source, and registers the native bar widget and conflict-checked shortcuts when available. It does not install OS packages, open firewall ports, load the compositor plugin, or start sharing automatically. Unowned or locally changed installed files are not overwritten.

To update, obtain the desired source revision and run `./install.sh` again. This stops an installed display session; start it explicitly afterwards. Update the TV receiver too: version 1.2's changed-region protocol replaces the former H.264/HLS transport and is intentionally incompatible with older receivers. Keep the installed app/package and original signing author identities to preserve TV pairing; host pairing is retained. To replace already-loaded native controls, run `omarchy-restart-shell` while the desktop is unlocked; do not use the settings-resetting `omarchy-refresh-shell`.

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

The receiver remembers the endpoint and secret **only after a complete image batch has been uploaded and drawn**, in that app's `localStorage`. Normally you enter them once; it reconnects on subsequent launches and when returning to the foreground. Its fit preference is also local. If TV storage fails, it reports session-only pairing. `localStorage` is not an encrypted vault. Remote Back disconnects and releases image/WebGL resources; returning to the foreground can reconnect using saved pairing. Disconnecting the TV does not stop the host output.

### Controls

Left-click the Omarchy monitor icon for Start/Stop, **Mirror / Extend** mode selection, numbered workspace selection, and next-start size/FPS. Mode and sizing changes never silently replace an active stream: stop and start explicitly. Bring back appears only in Extend. Pairing opens a private terminal only after an explicit click; credentials never appear in status or the bar.

The icon remains visible when stopped. A hollow accent dot means ready with no viewer; a filled dot means the host reports an authenticated capture viewer, **not measured playback quality**. Error and starting states are distinct. Status is polled, not instantaneous. See [bar widget details](omarchy-plugin/README.md).

`SUPER+CTRL+ALT+1` through `9` select that workspace in the **current mode**; `SUPER+CTRL+ALT+0` selects workspace **10**. The installer checks conflicts and migrates only its exact, unedited v1.1 shortcut block. The workspace must already exist and the host must be started.

```sh
omarchy-display mode mirror       # Save preferred mode for the next start
omarchy-display start
omarchy-display workspace 3       # Mirror existing workspace 3; no windows move
omarchy-display mirror 3          # Explicit alias; rejects an active Extend session
omarchy-display status            # Safe summary; no pairing credential
omarchy-display status --json
omarchy-display menu              # Interactive terminal alternative
omarchy-display stop
```

To use extension instead: stop, run `omarchy-display mode extend`, then start. `omarchy-display extend 3` requires an active Extend session; `omarchy-display recall 3` brings back a workspace transferred by that session. Mirror rejects recall and window-move operations because it never transferred those windows.

In Extend only, `omarchy-display move WINDOW_ADDRESS` moves one window using its real `hyprctl clients -j` address. Bring back is limited to workspaces with a recorded original monitor. Mirror preserves source ownership and aspect ratio, adding black borders where the source and capture aspect ratios differ.

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

The private capture connection advertises only `ODR3` (`0x4f445233`). A GLES 3.1 shader compares exact RGB values in 64×64 tiles against each viewer's last encoded image and classifies uniform colours in the same pass. Only compact flags/reference colours are read back, not full-frame pixels. Matching uniform tiles become rectangles carrying four opaque RGBA bytes. Changed non-uniform content uses unchanged TurboJPEG quality 90 and 4:4:4 chroma. Sparse rectangles map directly; fragmented plans may be packed by nearest GPU blits into reused linear GBM atlases before JPEG encoding. Eight-pixel placement and replicated edge padding preserve independent JPEG block results. Atlas selection must at least halve image count and limit decoded area to 1.25× the JPEG source area. Source pixels, resolution and quality are not reduced.

Authenticated batches have a 32-byte header and 16-byte record entries: big-endian unsigned 16-bit `x,y,width,height`, then unsigned 32-bit kind and payload length. Kind `0` is JPEG; kind `1` is four RGBA bytes with alpha 255. Kind `2` contains an 8-byte atlas header (`width,height,mapCount,reserved=0`), 12-byte maps (`sourceX,sourceY,width,height,atlasX,atlasY`) and one JPEG. Atlas record boxes bound their maps; only actual mapped source rectangles count toward coverage/non-overlap. At most 64 source rectangles and 32 MiB compressed data are accepted; the first batch covers the complete source. JPEG dimensions and the cumulative 1.25-frame decode-area budget are checked before image loading. Both ends require ODR3, without older-format compatibility decoders.

Mirror invalidations track selected windows, source layers and their subsurfaces/popups, geometry and eligible fadeouts. Viewer demand releases pending selected-client/layer frame callbacks; native fullscreen suppression is evaluated for the selected workspace without altering physical animation state, lifecycle fades or opacity rules. Static content needs no recurring capture timer. Three leased offscreen DMA-BUF slots stay immutable until native consumers release them, and GPU fences precede encoder access. A private readiness barrier ensures the requested source frame is installed before RFB capture. Native workers are joined and outstanding work retired before frame pools/devices are destroyed. Extend keeps its independent-output compositor path.

Each authenticated viewer has its own native difference baseline. At most two viewers are accepted. There is at most one unacknowledged image batch per viewer; the next update is requested only after the receiver uploads, draws and completes GPU work for the previous batch. The tested WebGL 1 device has no asynchronous fence extension, so completion uses `gl.finish()`. Slow viewers therefore do not build a movie queue or consume another viewer's changes. An unchanged image does not trigger another TV draw, and a static image has no playback-progress timeout.

The receiver retains a full-source texture/drawing buffer and queues a bounded batch of native JPEG image loads. Exact fills use solid-colour GPU quads on that source texture: the tested Mali driver stalled at completion after scissored clears, so error checks are retained rather than bypassed. Each atlas needs one native-image upload to a lazily grown, reused staging texture, then nearest GPU copies place its mapped rectangles; staging is bounded by padded source dimensions (about 32 MiB at native 4K). Total JPEG decoded area, including padding, is bounded to 1.25 source frames. Complete batches draw immediately and call `gl.finish()` before ACK; host pacing/backpressure replaces an extra animation-frame queue. Image loading is not assumed to finish decoding: older engines can defer work until texture upload. The verified TV uses **ARM Mali-450 MP** and a 1920×1080 app graphics plane; a 3840×2160 backing buffer is not proof of physical 4K detail.

Broadwell's hardware H.264 encoding worked in device experiments, but the tested Samsung native-video paths accumulated seconds of playback buffering. Broadwell does not provide hardware JPEG encoding. The shipped path uses exact GPU fills for uniform areas and explicit CPU JPEG compression for non-uniform areas, not a hidden video/software fallback. CPU and integrated GPU share a thermal package; GPU offload alone does not establish lower temperature.

Measure competing load separately: the reference Omarchy animated screensaver requests 120 FPS in Foot and can consume substantial CPU with sharing stopped. Record whether it remained active throughout a measurement; do not attribute its work to capture. The installer does not change screensaver, idle or lock settings.

Check the running path with `omarchy-display status` and temperature with `sensors`. Status counts a viewer only after a draw acknowledgement. Capture-to-ACK is source-frame age at acknowledgement, not physical panel latency; an old static frame on first connection can have a large age without large transport delay. On the TV, `OmarchyDisplayQA.status()` exposes source/backing dimensions, draw progress, decoded JPEG/atlas-image counts, GPU-fill counts and renderer errors. `OmarchyDisplayQA.requestPixels([{x:768,y:800}])` samples the actual default WebGL framebuffer after a draw, with source coordinates and sequence/PTS, without inferring panel resolution.

Hardware smoke checks are separate from syntax/build checks: select visibly different workspaces and inspect actual receiver pixels; confirm Mirror adds no monitor and transfers no windows; check conditional cursor clearing, inactive unfullscreen, shared-layer updates, large/small atlas reuse, Back/reconnect and worker shutdown. Exercise Extend and Bring back separately. Full-scene changes remain more expensive than small desktop updates; no smooth 4K full-motion, sustained temperature or glass-to-glass latency guarantee is made.

### Verified native-4K device samples

Controlled samples on the host/TV above, retaining quality 90/4:4:4. These are receiver upload/draw-completion measurements, not physical panel latency or a full-motion frame-rate promise.

| Measurement | Before batching/quad fills | Optimized path |
| --- | ---: | ---: |
| Native JPEG uploads, dense desktop batch | 39–40 | 1 |
| Dense batch native-image upload, one sample | 1,457 ms | 265 ms |
| Fill-only receive-to-draw ACK, one sample | 218 ms | 3.4 ms |
| Sustained sparse-marker receive-to-ACK P95 | 381 ms | 10.4 ms |

The GPU atlas matched all 8,110,080 stable desktop pixels byte-for-byte after JPEG decoding; a separate real-GPU proof matched 48 independently encoded crops, including partial eight-pixel edges, unaligned source origins and both RGB channel layouts.

A 20-second sparse-marker run changed the source 109 times:

| Process | CPU, percent of one core |
| --- | ---: |
| Native capture/encoder | 1.30 |
| Bridge | 1.65 |
| Controller, including waited helper children | 2.70 |
| Compositor, including physical desktop/test-fixture work | 20.24 |

Package temperature was 64–67°C in that short run. The screensaver process did not remain active throughout it; this is not a sustained-temperature result with the screensaver running. Screensaver and lock settings were unchanged.


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

For rollback, uninstall the currently installed version **without** `--purge-config`, then run the installer from a separate checkout of the desired earlier tag and start explicitly. This lets the current controller clear/unload its own compositor plugin before older files replace it. Rollback to `v1.1.0` was exercised with byte-identical pairing configuration and unrelated compositor plugins preserved; that version always uses extension, not true mirroring.

## Source and attribution

The host installer is also the source-build entry point. For a standalone capture build, `python3 desktop/build-wayvnc.py` uses the pinned sources/tools and native dependencies above, writes to the application XDG cache, and does not install or start the server. `npm ci --prefix desktop` installs the bridge's locked dependencies; the TV receiver needs no JavaScript bundle build. Generated files and packages are not source.

The project began from [CleanFrame](https://github.com/nthnclrk/CleanFrame/tree/fa537d1892b71d2de2bf94c24cbff3812e9f70d5), commit `fa537d1892b71d2de2bf94c24cbff3812e9f70d5`, by Nathan Clark (2026), under the [MIT license](LICENSE). CleanFrame provided a remote-oriented Samsung home-screen launcher, app launching, layouts/settings, wallpaper and clock UI, ES5 runtime conventions and TV setup groundwork. It did **not** provide this independent Linux workspace extender or the Jellyfin compatibility work. Its reference TV differs from this project's tested device; its compatibility and native Home/startup behavior are not inherited promises here.

The display adds host lifecycle/workspace controls, true workspace mirroring and explicit extension, output-lock integration, GPU changed-region encoding, an authenticated presentation-paced bridge, the ES5 WebGL receiver, pairing persistence, and Omarchy controls. The render-hook/offscreen capture approach is adapted from [hyprexpo](https://github.com/sandwichfarm/hyprexpo/tree/62fc27b8d6e0ff2eb0b97fa0568a91ad5d4239d2); its BSD notice is retained in [mirror-plugin/src/NOTICE](mirror-plugin/src/NOTICE). Existing compositor rendering, DMA-BUF capture, libjpeg-turbo and browser graphics APIs are not original inventions of this project.

The [Jellyfin integration](integrations/jellyfin-tv/README.md) is the full existing **Jellyfin Web 12.2** application plus the official [Jellyfin Tizen wrapper](https://github.com/jellyfin/jellyfin-tizen), adapted for the older engine—not a media client authored from scratch. Its build requirements and compatibility limits are separate from the desktop receiver's.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for component provenance, licenses and modifications. Third-party code retains its own license; the root MIT license does not relicense it. Original commercial-channel artwork, private development history, owner certificates and signed widgets are not part of the public source. This is an independent project, not affiliated with Samsung, Omarchy or Jellyfin.
