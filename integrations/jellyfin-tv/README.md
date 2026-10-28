# Jellyfin TV integration

This is **the complete existing Jellyfin Web 12.2.0 interface and official Jellyfin Tizen wrapper**, with a small build-time compatibility adaptation for an old TV browser. It is not a newly authored Jellyfin client, a server, or a codec implementation. Jellyfin is a separate optional TV application, not the desktop-display receiver.

## Requirements, before building

- **Node.js >=24.0.0 and npm >=11.0.0** (the pinned Jellyfin Web engine requirements).
- **Python >=3.12**, including its standard library and safe tar extraction support.
- Git, network access to the pinned GitHub sources and their npm dependencies, and sufficient local disk space for both builds and retained corresponding-source trees.
- Separately, for installation: your own Tizen SDK with Web CLI, SDB and appropriate Samsung TV extensions, a valid Samsung TV signing profile that authorizes your own device, and developer-mode installation permission. These are not needed just to build an unsigned widget.

There are no Python package dependencies. The builder fetches upstream sources and performs local npm builds; it does not contact a TV or Jellyfin server, modify network/server settings, sign a widget, install an SDK, or obtain certificates. Upstream npm lifecycle scripts execute locally as part of the build; review the pinned projects before running them.

From the repository root:

```sh
python3 integrations/jellyfin-tv/build.py
python3 tv.py package --app .local/jellyfin-build/widget --output dist/jellyfin-unsigned.wgt
unzip -l dist/jellyfin-unsigned.wgt
unzip -p dist/jellyfin-unsigned.wgt config.xml
```

Generated repositories, patched sources, dependency trees and payloads stay under the ignored `.local/jellyfin-build/`. Each build prints its retained `source-*` directory; previous builds are not reset or deleted. The current static payload is `.local/jellyfin-build/widget`. Do not point the packager at this integration's source directory: it deliberately rejects builders and requires the actual built interface and `tizen.js`.

The generated payload contains `BUILD.json`, both dependency lockfiles under `licenses/`, the project license texts, and collected npm dependency notices. It does not contain a server address, account, token, certificate, or preconfigured login. Choose your own server in the upstream Jellyfin interface.

## Your own signing and explicit installation

First follow Samsung's [certificate-profile instructions](https://developer.samsung.com/smarttv/develop/getting-started/setting-up-sdk/creating-certificates.html) and [Web CLI prerequisites](https://developer.samsung.com/smarttv/develop/getting-started/using-sdk/command-line-interface.html), including authorizing your device and permitting installation. Configure the profile in your SDK's Certificate Manager; the helper does not discover certificates, import profiles, request account passwords or accept password arguments. The SDK must already know how to unlock your profile securely.

**Tizen 3.0 is a legacy platform. Current SDK/TV-extension availability and generation of a valid, legacy-compatible Samsung distributor certificate are not guaranteed.** A configured profile is a prerequisite, not a promise that its certificate will work on every TV. Supply a profile you have independently confirmed is valid for your device; this project does not fall back to expired SDK distributor certificates. Do not weaken trust settings, change firmware, or root a TV to use these tools.

Replace the SDK paths, profile name and `TV_ADDRESS` with your own values:

```sh
python3 tv.py sign --input dist/jellyfin-unsigned.wgt --output dist/jellyfin-signed.wgt --tizen /absolute/path/to/tizen-studio/tools/ide/bin/tizen --profile YOUR_TV_PROFILE
python3 tv.py install --widget dist/jellyfin-signed.wgt --target TV_ADDRESS --tizen /absolute/path/to/tizen-studio/tools/ide/bin/tizen --sdb /absolute/path/to/tizen-studio/tools/sdb
python3 tv.py launch --application-id OmarchyJF1.Jellyfin --target TV_ADDRESS --sdb /absolute/path/to/tizen-studio/tools/sdb
```

`TV_ADDRESS` is an explicit IPv4 address, optionally with `:PORT`; the default SDB port is 26101. `--target` is the helper's address option: internally it uses SDK `tizen install --serial ADDRESS`, not the SDK's similarly named device-name option. Installation stages a reusable `update.wgt` basename and announces the app update before touching the TV. Launch uses `sdb -s ADDRESS shell 0 was_execute APPLICATION_ID`. Both require successful subprocess exit and exact application-specific success messages; unknown output is reported as unconfirmed, never assumed successful. Raw SDK output is not logged because it can expose local certificate/profile details. Private signing/install staging is retained under `$XDG_CACHE_HOME/omarchy-display/tv/` (default `~/.cache/omarchy-display/tv/`) with user-only permissions.

The generic public identity is `OmarchyJF1.Jellyfin`, package `OmarchyJF1`. To update an already installed private identity **without changing checked-in source**, supply your own existing IDs when packaging:

```sh
python3 tv.py package --app .local/jellyfin-build/widget --output dist/jellyfin-update-unsigned.wgt --application-id "$EXISTING_APPLICATION_ID" --package-id "$EXISTING_PACKAGE_ID"
```

Both override arguments are required together. Only `tizen:application` in the temporary payload changes; author metadata and source files do not. Sign using the **same original signing author identity** and matching IDs for an in-place update. Changing an XML author label cannot replace the signing author certificate. The helper does not uninstall, reset app data, reboot, restart firmware, enable autostart or remove staging directories. App-data preservation depends on the platform accepting an actual update; no helper can guarantee it after identity changes, external resets or certificate replacement. Never publish your certificates, profile configuration or signed per-device widget.

`python3 tv.py --help` and `python3 tv.py COMMAND --help` document all accepted arguments. Packaging is deterministic and unsigned: sorted static members, fixed timestamps and file modes, no inherited filesystem metadata. Symlinks, hidden files, `node_modules`, certificates, existing signatures and unexpected source/build files are rejected rather than silently shipped.

## Exact upstream provenance and local changes

| Component | Pinned source | License |
| --- | --- | --- |
| Jellyfin Web 12.2.0 | [`61b1f890365ad15138e943e694af723c1d47df2a`](https://github.com/jellyfin/jellyfin-web/tree/61b1f890365ad15138e943e694af723c1d47df2a) | GPL-2.0-or-later; [upstream license](https://github.com/jellyfin/jellyfin-web/blob/61b1f890365ad15138e943e694af723c1d47df2a/LICENSE), local `licenses/jellyfin-web.LICENSE` |
| Official Jellyfin Tizen wrapper 0.1.0 | [`31408c121e34e73c9f50c4e201c05e95140675df`](https://github.com/jellyfin/jellyfin-tizen/tree/31408c121e34e73c9f50c4e201c05e95140675df) | MPL-2.0; [upstream license](https://github.com/jellyfin/jellyfin-tizen/blob/31408c121e34e73c9f50c4e201c05e95140675df/LICENSE), local `licenses/jellyfin-tizen.LICENSE` |

The builder archives those exact Git objects into fresh directories; no private fork or Git history is copied. npm uses each upstream lockfile. Local compatibility changes are explicit and fail on a pinned-source text mismatch:

- `compat/themes.ts` retains all seven upstream palettes but asks MUI/Emotion to emit literal palette values, not runtime CSS custom properties unsupported by Chromium 47. The builder adapts consumers, root theme providers and independent legacy views accordingly.
- PostCSS resolves static custom-property fallbacks at build time, with a fallback for the book-player app bar. There is no runtime CSS-variable emulation.
- A small number of unsupported flex `gap` uses become margins. MUI CardHeader/ListItemText `:where(...)` selectors are rewritten in both installed module variants before webpack runs, because Emotion would otherwise emit them at runtime.
- The archived source has no `.git`; build revision metadata is the explicit upstream pin plus `-tizen3`, not a containing checkout's revision. System fonts and the `12.2.0-tizen3` version label are selected at build time.
- `compat/status.js` adds an inspector-only, pull-based `window.JellyfinTVStatus`. `snapshot()` reports coarse local state without identifiers, URLs or tokens. Only an explicit `probe()` call requests the selected server's unauthenticated `/System/Info/Public` endpoint; there is no background probe or logging. A local logged-in flag is not server-side authentication verification.
- Generic widget identity, entry redirect, Jellyfin icon, full license texts and generated dependency provenance are included. Playback remains upstream Samsung/native HTMLMediaElement behavior; no replacement player, server or codec path is invented here.

### Corresponding source and redistribution

The main repository's MIT license **does not relicense Jellyfin**. The theme adaptation and status module are GPL-2.0-or-later. Retain upstream notices and the GPL/MPL license texts. If distributing a generated Jellyfin widget, satisfy GPL corresponding-source requirements for the complete covered build, including the exact upstream sources, local modifications, dependency sources required by their licenses, lockfiles and build/install scripts. MPL-covered wrapper sources and modifications must likewise remain available as required by MPL. A generic upstream homepage or this notice alone is not a corresponding-source offer.

To support that obligation, each build retains the exact patched `web/` and `tizen/` source trees and installed dependency trees, plus snapshots of this `integration/` and the shared `tv.py`, in the printed ignored `source-*` directory. Preserve and provide the matching source with any binary distribution under the applicable license terms; inspect it for local build-machine information before publishing. For source-only redistribution, keep the immutable upstream links, local adapters and build instructions intact. The retained tree is not automatically uploaded or published.

## Compatibility evidence and limits

The only real-TV evidence is a Samsung **UE40MU6120, Tizen 3.0, firmware T-KTMDEUC 1410.1**. Earlier playback evidence covered **one 3840×2160 clip**, advancing playback time, pause, seek, resume and Back navigation. That is not full codec, audio, subtitle, HDR, DRM, transcoding, server-version or interface-feature coverage. Other Samsung models, firmware and platform generations remain unverified. A reported 1920×1080 application/display surface does not identify the physical panel's native resolution. This integration does not make a real-time desktop-display performance claim.
