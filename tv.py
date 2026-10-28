#!/usr/bin/env python3
"""Package, explicitly sign, install or launch a Samsung TV web widget.

Python standard library only. SDK operations require the caller's SDK binaries;
no certificates, passwords, profile discovery or TV operations are implicit.
Run `python3 tv.py COMMAND --help` for the complete argument contract.
"""
from __future__ import annotations

import argparse
import ipaddress
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parent
WIDGET_NS = "http://www.w3.org/ns/widgets"
TIZEN_NS = "http://tizen.org/ns/widgets"
APP_ID = re.compile(r"[A-Za-z0-9]{10}\.[A-Za-z0-9_]+\Z")
PACKAGE_ID = re.compile(r"[A-Za-z0-9]{10}\Z")
STATIC_EXTENSIONS = {
    ".html", ".htm", ".js", ".css", ".json", ".xml", ".txt", ".svg",
    ".png", ".jpg", ".jpeg", ".gif", ".webp", ".avif", ".ico", ".woff",
    ".woff2", ".ttf", ".otf", ".eot", ".wasm", ".mp3", ".ogg", ".wav",
    ".mp4", ".webm", ".vtt", ".srt", ".webmanifest", ".map", ".mjs", ".data", ".mem",
}
PRIVATE_EXTENSIONS = {".p12", ".pfx", ".pem", ".key", ".crt", ".cer", ".der", ".csr", ".p7b", ".p7c", ".jks", ".keystore"}


def fail(message: str) -> None:
    raise ValueError(message)


def allowed_name(name: str, signed: bool = False, directory: bool = False) -> None:
    path = PurePosixPath(name)
    if not name or "\\" in name or path.is_absolute() or any(
        part.lower() in ("", ".", "..", "node_modules", "certs", "certificates", "signatures") or part.startswith(".")
        for part in path.parts
    ) or str(path) != name:
        fail("Unsafe or unexpected payload path: " + name)
    lower = path.name.lower()
    if path.suffix.lower() in PRIVATE_EXTENSIONS or any(
        token in lower for token in ("certificate", "profiles.xml", "distributor", "author-signature")
    ) or re.fullmatch(r"signature\d*\.xml", lower):
        if signed and len(path.parts) == 1 and (lower == "author-signature.xml" or re.fullmatch(r"signature\d+\.xml", lower)):
            return
        fail("Certificate, profile or signature is not an unsigned static asset: " + name)
    if directory:
        return
    notice = any(token in lower for token in ("license", "licence", "copying", "notice", "authors", "ofl")) and path.suffix.lower() in ("", ".txt", ".md", ".rst", ".license", ".licence", ".mit", ".bsd", ".apache", ".gpl", ".0", ".1")
    if path.suffix.lower() not in STATIC_EXTENSIONS and not notice:
        fail("Not a recognized static asset: " + name)
    if lower.startswith("readme") or lower in ("package.json", "package-lock.json"):
        # Generated dependency locks are provenance, not executable source trees.
        if "licenses" not in path.parts:
            fail("Source/build metadata is not a widget asset: " + name)


def identity(config: bytes) -> tuple[ET.Element, str, str]:
    root = ET.fromstring(config)
    if root.tag != "{" + WIDGET_NS + "}widget":
        fail("config.xml is not a W3C widget")
    app = root.find("{" + TIZEN_NS + "}application")
    if app is None:
        fail("config.xml has no tizen:application")
    application, package = app.get("id", ""), app.get("package", "")
    if not APP_ID.fullmatch(application) or not PACKAGE_ID.fullmatch(package) or not application.startswith(package + "."):
        fail("Application ID must be PACKAGEID.Name with a matching ten-character package ID")
    return root, application, package


def require_payload(files: dict[str, bytes]) -> tuple[str, str]:
    for name in ("config.xml", "index.html", "icon.png"):
        if not files.get(name):
            fail("Missing required static widget file: " + name)
    if "www/index.html" in files:
        if not files.get("tizen.js"):
            fail("Jellyfin payload requires the built official wrapper tizen.js")
        roots = {"config.xml", "index.html", "icon.png", "tizen.js", "BUILD.json", "www", "licenses"}
    else:
        for name in ("main.js", "style.css", "LICENSE"):
            if not files.get(name):
                fail("Missing required static receiver file: " + name)
        roots = {"config.xml", "index.html", "icon.png", "main.js", "style.css", "LICENSE"}
    roots.update(("author-signature.xml", "signature1.xml"))
    for name in files:
        if PurePosixPath(name).parts[0] not in roots:
            fail("Unexpected file outside the static application whitelist: " + name)
    _, application, package = identity(files["config.xml"])
    return application, package


def read_payload(directory: Path) -> dict[str, bytes]:
    if directory.is_symlink() or not directory.is_dir():
        fail("--app must be a real static payload directory, not a symlink")
    files = {}
    for current, dirs, names in os.walk(directory, followlinks=False):
        for name in dirs + names:
            path = Path(current) / name
            relative = path.relative_to(directory).as_posix()
            if path.is_symlink():
                fail("Symlinks are forbidden in widget payloads: " + relative)
            if name.startswith(".") or name == "node_modules":
                fail("Unexpected hidden/build directory or file: " + relative)
            if path.is_dir():
                allowed_name(relative, directory=True)
                continue
            if not stat.S_ISREG(path.stat().st_mode):
                fail("Non-regular widget asset: " + relative)
            allowed_name(relative)
            files[relative] = path.read_bytes()
    require_payload(files)
    return files


def read_widget(path: Path, signed: bool = False) -> dict[str, bytes]:
    files = {}
    with zipfile.ZipFile(path) as archive:
        for member in archive.infolist():
            if member.is_dir():
                mode = member.external_attr >> 16
                if stat.S_IFMT(mode) not in (0, stat.S_IFDIR):
                    fail("Non-directory ZIP entry: " + member.filename)
                allowed_name(member.filename[:-1], directory=True)
                continue
            allowed_name(member.filename, signed=signed)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG)):
                fail("Non-regular ZIP entry: " + member.filename)
            if member.filename in files or member.flag_bits & 1:
                fail("Duplicate or encrypted widget entry: " + member.filename)
            files[member.filename] = archive.read(member)
    require_payload(files)
    if signed and not {"author-signature.xml", "signature1.xml"}.issubset(files):
        fail("Signed widget requires author-signature.xml and signature1.xml")
    return files


def atomic_output(output: Path, writer) -> None:
    output = output.expanduser().absolute()
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, name = tempfile.mkstemp(prefix="." + output.name + "-", dir=output.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            writer(stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)


def package(args) -> None:
    files = read_payload(args.app.expanduser())
    if bool(args.application_id) != bool(args.package_id):
        fail("Pass --application-id and --package-id together")
    if args.application_id:
        root, _, _ = identity(files["config.xml"])
        app = root.find("{" + TIZEN_NS + "}application")
        app.set("id", args.application_id)
        app.set("package", args.package_id)
        ET.register_namespace("", WIDGET_NS)
        ET.register_namespace("tizen", TIZEN_NS)
        # Change only application identity in the in-memory temporary payload.
        # Author metadata and all source files remain untouched.
        files["config.xml"] = ET.tostring(root, encoding="utf-8", xml_declaration=True)
    application, package_id = require_payload(files)
    def write_archive(stream):
        with zipfile.ZipFile(stream, "w", compression=zipfile.ZIP_STORED) as archive:
            for name in sorted(files):
                member = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
                member.create_system = 3
                member.external_attr = (stat.S_IFREG | 0o644) << 16
                archive.writestr(member, files[name])
    atomic_output(args.output, write_archive)
    print("Unsigned widget: " + str(args.output))
    print("Application: " + application + "; package: " + package_id)
    print("No signing or TV operation performed. Inspect with: unzip -l WIDGET.wgt")


def executable(value: Path) -> str:
    value = value.expanduser()
    if not value.is_absolute() or not value.is_file() or not os.access(value, os.X_OK):
        fail("SDK binary must be an explicit absolute executable path: " + str(value))
    return str(value)


def stage(label: str) -> Path:
    cache = Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "omarchy-display/tv"
    cache.mkdir(parents=True, exist_ok=True, mode=0o700)
    if cache.is_symlink() or cache.stat().st_uid != os.getuid():
        fail("TV staging directory must be real and owned by this user")
    cache.chmod(0o700)
    result = Path(tempfile.mkdtemp(prefix=label + "-", dir=cache))
    print("Private staging retained: " + str(result), flush=True)
    return result


def sdk_run(argv: list[str]) -> str:
    # Do not echo argv, SDK output or environment: SDK diagnostics may contain
    # local profile/certificate details. Caller can diagnose using their SDK.
    result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        fail("SDK operation failed (exit " + str(result.returncode) + "); no success claimed. Diagnose directly with your SDK; staging is retained.")
    return result.stdout


def sign(args) -> None:
    tizen = executable(args.tizen)
    if not args.profile.strip() or args.profile.startswith("-") or any(c in args.profile for c in "\r\n\0"):
        fail("--profile must be an explicit existing SDK security profile name")
    files = read_widget(args.input.expanduser())
    work = stage("sign")
    payload = work / "payload"
    payload.mkdir(mode=0o700)
    for name, content in files.items():
        destination = payload / name
        destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        destination.write_bytes(content)
    print("Signing with your explicit SDK profile; existing-app updates require its original author identity.", flush=True)
    sdk_run([tizen, "package", "--type", "wgt", "--sign", args.profile, "--", str(payload)])
    widgets = list(payload.glob("*.wgt"))
    if len(widgets) != 1:
        fail("SDK did not produce exactly one signed widget; inspect private staging")
    signed = read_widget(widgets[0], signed=True)
    unsigned = {name: value for name, value in signed.items() if name not in ("author-signature.xml", "signature1.xml")}
    if unsigned != files:
        fail("SDK altered static payload or produced unexpected signatures; refusing output")
    atomic_output(args.output, lambda stream: stream.write(widgets[0].read_bytes()))
    print("Signed widget: " + str(args.output) + ". No TV operation performed.")


def target_address(value: str) -> str:
    # Samsung developer-mode SDB uses an explicit IP and port, not a guessed TV.
    try:
        host, separator, port = value.rpartition(":")
        if not separator:
            host, port = value, "26101"
        address = ipaddress.IPv4Address(host)
        if not port.isdecimal() or not 1 <= int(port) <= 65535:
            raise ValueError
    except ValueError:
        raise argparse.ArgumentTypeError("Use an explicit IPv4 address, optionally :SDB_PORT (default 26101)")
    return str(address) + ":" + str(int(port))


def connect(sdb: str, target: str) -> None:
    sdk_run([sdb, "connect", target])
    output = sdk_run([sdb, "devices"])
    if not any(len(parts := line.split()) >= 2 and parts[0] == target and parts[1] == "device" for line in output.splitlines()):
        fail("Requested target is not an authorized SDB device")


def install(args) -> None:
    tizen, sdb = executable(args.tizen), executable(args.sdb)
    files = read_widget(args.widget.expanduser(), signed=True)
    _, application, _ = identity(files["config.xml"])
    work = stage("install")
    # A short stable ASCII filename avoids SDK basename/path upload mismatches.
    shutil.copyfile(args.widget.expanduser(), work / "update.wgt")
    print("TV APP UPDATE: installing on explicit target " + args.target + ". The foreground app may close; relaunch afterward.", flush=True)
    print("No uninstall/reset is performed. Matching app/package AND original signing author are required to preserve pairing.", flush=True)
    connect(sdb, args.target)
    output = sdk_run([tizen, "install", "--name", "update.wgt", "--serial", args.target, "--", str(work)])
    lines = {line.strip() for line in output.splitlines()}
    markers = {"app_id[" + application + "] install completed", "cmd_ret:0", "Installed the package: Id(" + application + ")", "Tizen application is successfully installed."}
    if not markers.issubset(lines):
        fail("SDK returned without the exact documented install success markers; installation outcome is unconfirmed")
    print("TV app update confirmed by SDK. Launch it explicitly with the launch command.")


def launch(args) -> None:
    if not APP_ID.fullmatch(args.application_id):
        fail("--application-id must be PACKAGEID.Name")
    sdb = executable(args.sdb)
    print("TV APP LAUNCH: the selected application will become visible on " + args.target, flush=True)
    connect(sdb, args.target)
    output = sdk_run([sdb, "-s", args.target, "shell", "0", "was_execute", args.application_id])
    lines = {line.strip() for line in output.splitlines()}
    if not lines.intersection({"app_id[" + args.application_id + "] launched", "app_id[" + args.application_id + "] resumed"}):
        fail("SDB returned without an exact application-specific launched/resumed line; launch outcome is unconfirmed")
    print("TV app launch confirmed by SDB.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Signing requires your OWN existing SDK security profile and a valid Samsung TV distributor certificate. No password/certificate arguments are accepted. Legacy TV certificate compatibility is not guaranteed. Staging is private and retained; no cleanup/uninstall/reset/restart is performed.")
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("package", help="Create a deterministic unsigned ZIP from a built static payload")
    command.add_argument("--app", type=Path, default=ROOT / "apps/desktop-tv", help="Built static widget directory (default: apps/desktop-tv)")
    command.add_argument("--output", type=Path, required=True, help="Atomic output .wgt path; parent directories are created")
    command.add_argument("--application-id", help="Temporary payload override; requires --package-id (source/author unchanged)")
    command.add_argument("--package-id", help="Matching ten-character package ID; requires --application-id")
    command.set_defaults(action=package)
    command = commands.add_parser("sign", help="Sign an unsigned widget using your explicit SDK/profile")
    command.add_argument("--input", type=Path, required=True, help="Unsigned widget from package")
    command.add_argument("--output", type=Path, required=True, help="Atomic signed output .wgt path (private; never publish)")
    command.add_argument("--tizen", type=Path, required=True, help="Absolute path to your Tizen SDK CLI executable")
    command.add_argument("--profile", required=True, help="Your already configured SDK security profile name; not a password")
    command.set_defaults(action=sign)
    command = commands.add_parser("install", help="Update the app using SDK tizen install; never uninstall")
    command.add_argument("--widget", type=Path, required=True, help="Signed widget matching the installed author's identity")
    command.add_argument("--tizen", type=Path, required=True, help="Absolute Tizen SDK CLI executable")
    command.add_argument("--sdb", type=Path, required=True, help="Absolute SDB executable from the same SDK")
    command.add_argument("--target", type=target_address, required=True, help="Explicit TV IPv4[:port]; default SDB port 26101")
    command.set_defaults(action=install)
    command = commands.add_parser("launch", help="Launch/resume with supported SDB shell 0 was_execute")
    command.add_argument("--application-id", required=True, help="Exact installed application ID, normally OmarchyTV1.Display")
    command.add_argument("--sdb", type=Path, required=True, help="Absolute SDK SDB executable")
    command.add_argument("--target", type=target_address, required=True, help="Explicit TV IPv4[:port]; default SDB port 26101")
    command.set_defaults(action=launch)
    args = parser.parse_args()
    os.umask(0o077)
    try:
        args.action(args)
    except (ValueError, OSError, ET.ParseError, zipfile.BadZipFile) as error:
        parser.exit(1, "tv.py: " + str(error) + "\n")


if __name__ == "__main__":
    main()
