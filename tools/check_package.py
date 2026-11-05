"""Verify that the portable ZIP and installer carry the complete local manual."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import re
import struct
import zipfile

ROOT = Path(__file__).resolve().parent.parent
header = (ROOT / "src/version.h").read_text(encoding="utf-8")
version = ".".join(re.search(rf"CYIMG_VERSION_{part} (\d+)", header)[1] for part in ("MAJOR", "MINOR", "PATCH"))
name = f"CyImgView-{version}-win64"
archive = ROOT / "dist" / f"{name}.zip"
installer = ROOT / "dist" / f"CyImgView-{version}-Setup.exe"
exported = ROOT / "build/Release/bin/help"

with zipfile.ZipFile(archive) as package:
    assert package.testzip() is None, "ZIP CRC verification failed"
    names = package.namelist()
    assert len(names) == len(set(names)), "Duplicate ZIP entries"
    for entry in names:
        path = PurePosixPath(entry.replace("\\", "/"))
        assert not path.is_absolute() and ".." not in path.parts, f"Unsafe package path: {entry}"
        assert path.parts[0] == name, f"Unexpected top-level path: {entry}"
        assert "node_modules" not in path.parts, "Build dependencies were packaged"
    packaged_help = {entry[len(name + '/help/'):] for entry in names if entry.startswith(name + "/help/") and not entry.endswith("/")}
    expected_help = {file.relative_to(exported).as_posix() for file in exported.rglob("*") if file.is_file()}
    assert packaged_help == expected_help, "Portable package differs from the complete help export"
    for relative in expected_help:
        assert package.read(f"{name}/help/{relative}") == (exported / relative).read_bytes(), f"Stale manual resource: {relative}"
    assert package.read(f"{name}/CyImgView.exe") == (ROOT / "build/Release/bin/CyImgView.exe").read_bytes(), "Stale native executable"
    info = json.loads(package.read(f"{name}/help/build-info.json"))
    assert info["version"] == version and info["modules"] == ["image", "gallery"], "Wrong manual build or modules"
    assert {path for path in packaged_help if re.fullmatch(r"media-.+\.js", path)} == {"media-image.js", "media-gallery.js"}
    assert len(info["sdk"]) == 9, "Unexpected source index scope"
    manifest = json.loads(package.read(f"{name}/help/manifest.json"))
    assert len(manifest["locales"]) == 12, "Lost manual languages"

payload = archive.read_bytes()
setup = installer.read_bytes()
assert setup[:2] == b"MZ", "Invalid installer stub"
assert setup[-16:-8] == b"CYIMGPK1", "Installer trailer missing"
length = struct.unpack("<Q", setup[-8:])[0]
assert length == len(payload), "Installer payload length mismatch"
assert setup[-16-length:-16] == payload, "Installer does not contain the verified ZIP"
print(f"Package checks: {version}; {len(packaged_help)} help files identical; {len(manifest['pages'])} pages; image + gallery; installer payload identical.")
print(f"ZIP SHA-256: {hashlib.sha256(payload).hexdigest()}")
