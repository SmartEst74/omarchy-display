# Third-party notices and source provenance

Omarchy Display integrates existing software; its project license does not replace third-party licenses. The public source tree contains build recipes rather than private generated bundles, signed widgets, SDK certificates or fork history. Preserve applicable copyright, license and source-availability notices when redistributing generated artifacts. Versions below distinguish current dependencies from the previously inspected noVNC receiver build. Historical source pins do not imply those components remain current runtime dependencies.

## CleanFrame origin

The original launcher work is **[CleanFrame](https://github.com/nthnclrk/CleanFrame)** by **Nathan Clark, copyright 2026**, pinned provenance commit [`fa537d1892b71d2de2bf94c24cbff3812e9f70d5`](https://github.com/nthnclrk/CleanFrame/tree/fa537d1892b71d2de2bf94c24cbff3812e9f70d5), under the [MIT License](https://github.com/nthnclrk/CleanFrame/blob/fa537d1892b71d2de2bf94c24cbff3812e9f70d5/LICENSE). Retain that copyright and permission notice for copied portions.

Omarchy Display adds workspace mirroring/extension controls, output-locked capture integration, asynchronous bridge, TV receiver/pairing flow, packaging tooling and Omarchy UI integration. Upstream launcher, compositor rendering, capture, and Jellyfin are not original inventions of this project. Commercial-channel artwork and private fork history are not part of this distribution.

## Workspace mirror rendering

`mirror-plugin/src/main.cpp` adapts the render-hook/offscreen capture approach and state-guard patterns from [sandwichfarm/hyprexpo](https://github.com/sandwichfarm/hyprexpo/tree/62fc27b8d6e0ff2eb0b97fa0568a91ad5d4239d2), inspected at commit `62fc27b8d6e0ff2eb0b97fa0568a91ad5d4239d2`, including `OverviewCapture.cpp`, `Overview.cpp`, and `main.cpp`. The upstream **BSD-3-Clause** notice, copyright 2023 Hypr Development, is retained in [`mirror-plugin/src/NOTICE`](mirror-plugin/src/NOTICE) and installed with the plugin source. Preserve it when distributing source or binaries; the root MIT license does not replace it.

The plugin builds against the host's exact Hyprland 0.56.2 headers and links host libraries. This repository does not redistribute a modified Hyprland executable. Hyprland and linked libraries retain their own upstream license terms.

## Capture stack

`desktop/build-wayvnc.py` identifies these immutable sources:

| Project | Version and commit | Upstream license |
| --- | --- | --- |
| [wayvnc](https://github.com/any1/wayvnc) | 0.10.2, [`bb837459c75fd3c267d48af170ac7ed2566f67e5`](https://github.com/any1/wayvnc/tree/bb837459c75fd3c267d48af170ac7ed2566f67e5) | [ISC `COPYING`](https://github.com/any1/wayvnc/blob/bb837459c75fd3c267d48af170ac7ed2566f67e5/COPYING) |
| [neatvnc](https://github.com/any1/neatvnc) | 1.0.3, [`783437adc56c2d7ce2cac1ec584acad798594267`](https://github.com/any1/neatvnc/tree/783437adc56c2d7ce2cac1ec584acad798594267) | [ISC `COPYING`](https://github.com/any1/neatvnc/blob/783437adc56c2d7ce2cac1ec584acad798594267/COPYING) |
| [aml](https://github.com/any1/aml) | 1.0.0, [`685035c9830aa89df02a43df89b644690bd885f5`](https://github.com/any1/aml/tree/685035c9830aa89df02a43df89b644690bd885f5) | [ISC `COPYING`](https://github.com/any1/aml/blob/685035c9830aa89df02a43df89b644690bd885f5/COPYING) |

All three pinned upstream `COPYING` files were inspected; they contain this notice:

> Copyright (c) 2019 - 2020 Andri Yngvason
>
> Permission to use, copy, modify, and/or distribute this software for any purpose
> with or without fee is hereby granted, provided that the above copyright notice
> and this permission notice appear in all copies.
>
> THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
> REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
> FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
> INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
> OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
> TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
> THIS SOFTWARE.

The builder changes wayvnc `src/main.c` to lock capture to the selected output and fail closed if it disappears or a switch would capture another output. It does not claim to author wayvnc or neatvnc. Meson 1.12.1, Ninja 1.13.0, and [CMake 3.31.6](https://pypi.org/project/cmake/3.31.6/) build tools use hash-pinned wheel requirements in a private environment; their own upstream licenses/notices remain applicable. Host library versions are recorded in the capture build receipt.

## Current image receiver and bridge

The desktop receiver uses static ES5 JavaScript and native browser image decoding. It does not bundle or require noVNC, JSBI, core-js, ResizeObserver, Babel or webpack. Its package consists of `config.xml`, `index.html`, `icon.png`, `main.js`, `style.css` and the required MIT `LICENSE` notice; there is no npm receiver-bundle build step. The bridge retains **ws 8.22.0, MIT**, with the [published package LICENSE](https://unpkg.com/ws@8.22.0/LICENSE): copyright Einar Otto Stangvik, Arnout Kazemier, Luigi Pinca and contributors.

The host image encoder uses the caller's installed FFmpeg executable, not an FFmpeg binary copied into this repository. FFmpeg's applicable LGPL/GPL terms depend on that executable's build configuration and linked components; retain the distributor's notices and consult the [upstream legal documentation](https://ffmpeg.org/legal.html) before redistributing an encoder build. No universal FFmpeg build license is asserted here.

## Historical noVNC receiver provenance

The following exact versions and licenses were inspected in the former receiver builder and its lockfile. They document the original integration and source adaptation, **not the current receiver's runtime dependency list**. The noVNC bundle and its browser-build tooling have been superseded by the native-image receiver. Jellyfin's independently pinned dependency tree and notices are unaffected.

| Component | Exact direct version | License and inspected source |
| --- | --- | --- |
| [noVNC](https://github.com/novnc/noVNC) | 1.7.0 (`@novnc/novnc`) | Core **MPL-2.0**; [v1.7.0 `LICENSE.txt`](https://github.com/novnc/noVNC/blob/v1.7.0/LICENSE.txt), [full MPL](https://github.com/novnc/noVNC/blob/v1.7.0/docs/LICENSE.MPL-2.0) |
| [JSBI](https://github.com/GoogleChromeLabs/jsbi) | 4.3.2 | **Apache-2.0**, [published package LICENSE](https://unpkg.com/jsbi@4.3.2/LICENSE) |
| [core-js](https://github.com/zloirock/core-js) | 3.51.0 | **MIT**, [published package LICENSE](https://unpkg.com/core-js@3.51.0/LICENSE); copyright Denis Pushkarev and CoreJS Company |
| [@juggle/resize-observer](https://github.com/juggle/resize-observer) | 3.4.0 | **Apache-2.0**, [published package LICENSE](https://unpkg.com/@juggle/resize-observer@3.4.0/LICENSE); copyright 2019 JUGGLE LTD |
| [Babel core](https://github.com/babel/babel) | 7.28.5 (`@babel/core`) | **MIT**, [published package LICENSE](https://unpkg.com/@babel/core@7.28.5/LICENSE); copyright Sebastian McKenzie and other contributors |
| Babel preset-env | 7.28.5 (`@babel/preset-env`) | **MIT**, [package license](https://unpkg.com/@babel/preset-env@7.28.5/LICENSE); version/license recorded by the lockfile |
| [babel-loader](https://github.com/babel/babel-loader) | 10.1.1 | **MIT**, [package license](https://unpkg.com/babel-loader@10.1.1/LICENSE); version/license recorded by the lockfile |
| [webpack](https://github.com/webpack/webpack) | 5.112.2 | **MIT**, [published package LICENSE](https://unpkg.com/webpack@5.112.2/LICENSE); copyright JS Foundation and other contributors |
| [Acorn](https://github.com/acornjs/acorn) | 8.19.0 | **MIT**, [package license](https://unpkg.com/acorn@8.19.0/LICENSE); version/license recorded by the lockfile |
| [ws](https://github.com/websockets/ws) | 8.22.0 | **MIT**, [published package LICENSE](https://unpkg.com/ws@8.22.0/LICENSE); copyright Einar Otto Stangvik, Arnout Kazemier, Luigi Pinca and contributors |

noVNC's upstream `LICENSE.txt` also identifies separately licensed material: `core/des.js` has BSD-style notices and vendored pako is MIT; upstream HTML/CSS, fonts and artwork have separate BSD, OFL and CC-BY-SA terms. The former receiver builder preserved upstream noVNC license/AUTHORS files, vendored notices and bundled dependency licenses. These historical assets are not required by the current native-image receiver. Do not assume a separately distributed noVNC package is MPL-only.

The former receiver integration used ES5/Chromium-47-targeted Babel/webpack output, a final ES5 grammar check, core-js and ResizeObserver support, a legacy CustomEvent fallback, and replacement of native BigInt syntax with real JSBI arithmetic. Its BigInt source excerpt was an **MPL-2.0 adaptation of noVNC's `core/crypto/bigint.js`**, originating at [commit `63107bd06d9e1f6136ff21aeda8cd62cbf0d433e`](https://github.com/novnc/noVNC/blob/63107bd06d9e1f6136ff21aeda8cd62cbf0d433e/core/crypto/bigint.js). This is retained historical attribution, not a claim that noVNC arithmetic or source excerpts ship in the current receiver.

If redistributing a historical noVNC bundle or adaptation separately, retain the MPL license and provide recipients the covered source form and modifications as MPL requires. A minified or transpiled bundle alone is not its preferred form for modification. Apache-licensed components retain their license and applicable NOTICE/attribution obligations. Removal from the current receiver does not relicense previously distributed covered code as MIT.

## Optional full Jellyfin application

The optional [`integrations/jellyfin-tv/`](integrations/jellyfin-tv/README.md) is the full existing Jellyfin interface plus the official TV wrapper, not an original client authored here:

| Component | Exact inspected source pin | License |
| --- | --- | --- |
| Jellyfin Web 12.2.0 | [`61b1f890365ad15138e943e694af723c1d47df2a`](https://github.com/jellyfin/jellyfin-web/tree/61b1f890365ad15138e943e694af723c1d47df2a) | **GPL-2.0-or-later**, [upstream LICENSE](https://github.com/jellyfin/jellyfin-web/blob/61b1f890365ad15138e943e694af723c1d47df2a/LICENSE) and [`package.json`](https://github.com/jellyfin/jellyfin-web/blob/61b1f890365ad15138e943e694af723c1d47df2a/package.json) declaration; copied as `integrations/jellyfin-tv/licenses/jellyfin-web.LICENSE` |
| Official Jellyfin Tizen wrapper 0.1.0 | [`31408c121e34e73c9f50c4e201c05e95140675df`](https://github.com/jellyfin/jellyfin-tizen/tree/31408c121e34e73c9f50c4e201c05e95140675df) | **MPL-2.0**, [upstream LICENSE](https://github.com/jellyfin/jellyfin-tizen/blob/31408c121e34e73c9f50c4e201c05e95140675df/LICENSE); copied as `integrations/jellyfin-tv/licenses/jellyfin-tizen.LICENSE` |

The local builder adapts MUI themes to literal palettes, resolves static CSS-variable fallbacks, rewrites unsupported selectors/flex gaps, adds a privacy-limited inspector status module, sets deterministic upstream revision metadata, and provides generic widget identity plus dependency provenance. `compat/themes.ts` and `compat/status.js` remain GPL-2.0-or-later. The Jellyfin icon identifies the upstream project; this integration does not claim trademark ownership or endorsement.

**Distributing a generated Jellyfin widget requires satisfying GPL corresponding-source obligations**, including the exact covered sources, modifications and scripts controlling compilation/installation, and applicable dependency-license obligations. The wrapper also requires MPL source availability. Merely pointing to an upstream homepage does not satisfy these requirements. Do not represent the complete bundled application as MIT-licensed because this repository's original tooling is MIT.

The builder retains the exact patched upstream/dependency trees, integration snapshot and shared packager in an ignored `.local/jellyfin-build/source-*` directory. Generated widgets contain both dependency lockfiles and collected notices, but **license texts and lockfiles alone are not complete corresponding source**. Preserve the matching retained tree and arrange compliant source distribution alongside any binary release; inspect build-machine metadata before publishing. See the integration README for precise build requirements and instructions. No source, SDK profile, signed widget or build directory is automatically published.
