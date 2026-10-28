#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the complete pinned Jellyfin client; never sign or contact a TV."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

APP = Path(__file__).resolve().parent
REPO = APP.parents[1]
BUILD = REPO / ".local/jellyfin-build"
SOURCES = {
    "web": ("https://github.com/jellyfin/jellyfin-web.git", "61b1f890365ad15138e943e694af723c1d47df2a"),
    "tizen": ("https://github.com/jellyfin/jellyfin-tizen.git", "31408c121e34e73c9f50c4e201c05e95140675df"),
}


def run(*args: str, cwd: Path, env: dict[str, str] | None = None) -> None:
    subprocess.run(args, cwd=cwd, env=env, check=True)


def replace(path: Path, old: str, new: str, count: int = 1) -> None:
    text = path.read_text()
    actual = text.count(old)
    if actual != count:
        raise RuntimeError(f"Pinned-source mismatch in {path.name}: expected {count}, found {actual}")
    text = text.replace(old, new)
    marker = "Omarchy Display compatibility adaptation, 2026-10-28; see integrations/jellyfin-tv/build.py."
    if marker not in text:
        text = "/* " + marker + " */\n" + text
    path.write_text(text)


def checkout(name: str, work: Path) -> Path:
    url, revision = SOURCES[name]
    cache = BUILD / "repositories" / name
    if not cache.exists():
        cache.parent.mkdir(parents=True, exist_ok=True)
        run("git", "clone", "--bare", "--filter=blob:none", url, str(cache), cwd=BUILD)
    run("git", "fetch", "--depth=1", "origin", revision, cwd=cache)
    archive = work / (name + ".tar")
    with archive.open("wb") as output:
        subprocess.run(["git", "archive", revision], cwd=cache, stdout=output, check=True)
    destination = work / name
    destination.mkdir()
    with tarfile.open(archive) as source:
        source.extractall(destination, filter="data")
    archive.unlink()
    return destination


def adapt(web: Path) -> None:
    src = web / "src"
    shutil.copyfile(APP / "compat/themes.ts", src / "themes/index.ts")
    shutil.copyfile(APP / "compat/status.js", src / "tizen3-status.js")
    replace(src / "index.jsx", "import 'lib/legacy';", "import 'lib/legacy';\nimport './tizen3-status';")
    palette_files = {
        "apps/modern/components/AppToolbar/RemotePlayButton.tsx": 1,
        "components/UserAvatar.tsx": 2,
        "components/mediainfo/StarIcons.tsx": 1,
        "elements/emby-progressbar/AutoTimeProgressBar.tsx": 2,
        "themes/_base/theme.ts": 1,
    }
    for filename, count in palette_files.items():
        replace(src / filename, "theme.vars.palette", "theme.palette", count)

    router = src / "RootAppRouter.tsx"
    replace(router, "import appTheme from 'themes';\nimport { ThemeStorageManager } from 'themes/themeStorageManager';", "import { useLegacyTheme } from 'themes';")
    replace(router, "    const location = useLocation();", "    const location = useLocation();\n    const appTheme = useLegacyTheme();")
    replace(router, "            defaultMode='dark'\n            storageManager={ThemeStorageManager}\n", "")

    react = src / "utils/reactUtils.tsx"
    replace(react, "import { type SupportedColorScheme, ThemeProvider, useColorScheme } from '@mui/material/styles';", "import { ThemeProvider } from '@mui/material/styles';")
    replace(react, "import React, { type FC, type PropsWithChildren, useEffect } from 'react';", "import React, { type FC, type PropsWithChildren } from 'react';")
    replace(react, "import appTheme from 'themes';", "import { useLegacyTheme } from 'themes';")
    start = react.read_text().index("/** Component that syncs the MUI theme")
    end = react.read_text().index("/** Component that provides a root context")
    old = react.read_text()[start:end]
    replace(react, old, """/** Emit literal MUI palette values, including for independently mounted legacy views. */
const CustomThemeProvider: FC<PropsWithChildren> = ({ children }) => {
    const { theme } = useUserTheme();
    const appTheme = useLegacyTheme(theme);
    return <ThemeProvider theme={appTheme}>{children}</ThemeProvider>;
};

""")
    replace(react, """                        <ThemeProvider
                            theme={appTheme}
                            // Disable color scheme management since we're handling it manually
                            colorSchemeNode={null}
                            storageManager={null}
                            disableStyleSheetGeneration
                        >
                            <CustomThemeProvider>
                                {children}
                            </CustomThemeProvider>
                        </ThemeProvider>""", """                        <CustomThemeProvider>
                            {children}
                        </CustomThemeProvider>""")

    # Resolve upstream Sass fallbacks at build time, not via a JS CSS-var ponyfill.
    replace(web / "postcss.config.js", "postcssPresetEnv({ browsers: packageConfig.browserslist })", "postcssPresetEnv({ browsers: packageConfig.browserslist, features: { 'custom-properties': { preserve: false } } })")
    replace(src / "plugins/bookPlayer/BookOsd/BookOsd.scss", "var(--jf-palette-AppBar-defaultBg)", "var(--jf-palette-AppBar-defaultBg, #202020)")
    # git archive deliberately has no .git directory. Do not accidentally pick
    # up the containing launcher's revision through git's parent-directory walk.
    webpack = web / "webpack.common.js"
    original = webpack.read_text()
    start = original.index("let COMMIT_SHA = '';")
    end = original.index("const NODE_MODULES_REGEX")
    replace(webpack, original[start:end], "const COMMIT_SHA = '" + SOURCES["web"][1] + "-tizen3';\n\n")
    replace(src / "components/playback/skipbutton.scss", "    gap: 3px;\n", "")
    with (src / "components/playback/skipbutton.scss").open("a") as output:
        output.write("\n.skip-button > * + * { margin-left: 3px; }\n")
    replace(src / "apps/dashboard/routes/devices/index.tsx", "                        gap: 1,", "                        '& > * + *': { marginLeft: 1 },")


def main() -> None:
    if sys.version_info < (3, 12):
        raise SystemExit("This builder requires Python >=3.12 (safe tar extraction)")
    if int(subprocess.check_output(["node", "-p", "process.versions.node.split('.')[0]"], text=True)) < 24:
        raise SystemExit("Jellyfin Web 12.2.0 requires Node.js >=24 and npm >=11")
    if int(subprocess.check_output(["npm", "--version"], text=True).split(".")[0]) < 11:
        raise SystemExit("Jellyfin Web 12.2.0 requires npm >=11")
    os.umask(0o077)
    BUILD.mkdir(parents=True, exist_ok=True, mode=0o700)
    # Retain the exact patched source/dependency tree for corresponding-source
    # distribution. Never reset a checkout or delete a previous build.
    work = Path(tempfile.mkdtemp(prefix="source-", dir=BUILD))
    print("Build and corresponding-source tree (retained): " + str(work))
    web = checkout("web", work)
    tizen = checkout("tizen", work)
    adapt(web)
    env = dict(os.environ, USE_SYSTEM_FONTS="1", JELLYFIN_VERSION="12.2.0-tizen3", SOURCE_DATE_EPOCH="1791240999")
    run("npm", "ci", "--no-audit", "--no-fund", cwd=web, env=env)
    # Emotion emits these MUI rules at runtime, outside PostCSS.
    mui = web / "node_modules/@mui/material"
    for component, slots in (("CardHeader", ("title", "subheader")), ("ListItemText", ("primary", "secondary"))):
        classes = component[0].lower() + component[1:] + "Classes"
        for variant in ("", "modern"):
            path = mui / variant / component / (component + ".js")
            for slot in slots:
                replace(path, "`.${typographyClasses.root}:where(& .${" + classes + "." + slot + "})`",
                        "`& .${" + classes + "." + slot + "}.${typographyClasses.root}`")
    run("npm", "run", "build:production", cwd=web, env=env)
    env["JELLYFIN_WEB_DIR"] = str(web / "dist")
    run("npm", "ci", "--no-audit", "--no-fund", cwd=tizen, env=env)
    stage = work / "widget"
    stage.mkdir()
    for filename in ("config.xml", "index.html", "icon.png"):
        shutil.copyfile(APP / filename, stage / filename)
    shutil.copyfile(tizen / "tizen.js", stage / "tizen.js")
    shutil.copytree(tizen / "www", stage / "www")
    shutil.copytree(APP / "licenses", stage / "licenses")
    # Keep both dependency lockfiles and all installed dependency notices.
    for name, source in (("web", web), ("tizen", tizen)):
        shutil.copyfile(source / "package-lock.json", stage / "licenses" / (name + ".package-lock.json"))
    dependency_notices = stage / "licenses/npm"
    dependency_notices.mkdir()
    for project, source in (("web", web), ("tizen", tizen)):
        modules = source / "node_modules"
        for directory, dirs, files in os.walk(modules):
            dirs[:] = [name for name in dirs if not name.startswith(".")]
            for filename in files:
                if filename.upper().startswith(("LICENSE", "LICENCE", "COPYING", "NOTICE", "OFL")):
                    notice = Path(directory) / filename
                    if notice.is_file() and not notice.is_symlink():
                        name = project + "__" + "__".join(notice.relative_to(modules).parts)
                        shutil.copyfile(notice, dependency_notices / name)
    # Snapshot this integration and shared packager beside the exact upstream
    # build trees, not references to a later moving local checkout.
    shutil.copytree(APP, work / "integration", ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    shutil.copyfile(REPO / "tv.py", work / "tv.py")
    provenance = {
        "webVersion": "12.2.0", "tizenVersion": "0.1.0",
        "sources": {name: {"url": url, "commit": commit} for name, (url, commit) in SOURCES.items()},
        "localSource": "integrations/jellyfin-tv/build.py and compat/ in the accompanying repository",
        "playback": "upstream native Samsung HTMLMediaElement with native MPEG-TS HLS on Tizen 3.0",
    }
    (stage / "BUILD.json").write_text(json.dumps(provenance, indent=2) + "\n")
    if not (stage / "www/index.html").is_file():
        raise RuntimeError("Upstream interface build did not produce www/index.html")
    destination = BUILD / "widget"
    if destination.exists():
        # Move the previous generated widget into this build's unique private
        # directory; never remove it or mutate a TV's application storage.
        destination.rename(work / "previous-widget")
    stage.rename(destination)
    print("Unsigned static payload: .local/jellyfin-build/widget")
    print("Package with: python3 tv.py package --app .local/jellyfin-build/widget --output dist/jellyfin-unsigned.wgt")
    print("No signing, credentials, server changes or TV operations were performed.")


if __name__ == "__main__":
    main()
