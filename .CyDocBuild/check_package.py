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
exported = ROOT / "build/Release/bin/Documentation"

with zipfile.ZipFile(archive) as package:
    assert package.testzip() is None, "ZIP CRC verification failed"
    names = package.namelist()
    assert len(names) == len(set(names)), "Duplicate ZIP entries"
    for entry in names:
        path = PurePosixPath(entry.replace("\\", "/"))
        assert not path.is_absolute() and ".." not in path.parts, f"Unsafe package path: {entry}"
        assert path.parts[0] == name, f"Unexpected top-level path: {entry}"
        assert not {"node_modules", ".CyDocBuild", "help"}.intersection(path.parts), "Private tooling or old help was packaged"
    packaged_help = {entry[len(name + '/Documentation/'):] for entry in names if entry.startswith(name + "/Documentation/") and not entry.endswith("/")}
    expected_help = {file.relative_to(exported).as_posix() for file in exported.rglob("*") if file.is_file()}
    assert packaged_help == expected_help, "Portable package differs from the complete help export"
    for relative in expected_help:
        assert package.read(f"{name}/Documentation/{relative}") == (exported / relative).read_bytes(), f"Stale manual resource: {relative}"
    assert package.read(f"{name}/CyImgView.exe") == (ROOT / "build/Release/bin/CyImgView.exe").read_bytes(), "Stale native executable"
    assert f"{name}/plugins/metadata_exiv2.dll" in names, "Native metadata provider missing"
    assert package.read(f"{name}/plugins/metadata_exiv2.dll") == (ROOT / "build/Release/bin/plugins/metadata_exiv2.dll").read_bytes(), "Stale metadata provider DLL"
    assert not any("/testplugins/" in entry or "/metadata-fixtures-" in entry for entry in names), "Test metadata artifacts were packaged"
    for notice in ("exiv2-COPYING", "exiv2-LICENSE.txt", "exiv2-doc-COPYING-XMPSDK", "expat-expat-COPYING", "zlib-LICENSE"):
        assert f"{name}/licenses/{notice}" in names, f"Metadata dependency license missing: {notice}"
    assert {entry.split('/')[0] for entry in packaged_help} == {"Documentation.html", "DocAI.md", "DocData"}, "Wrong export layout"
    info = json.loads(package.read(f"{name}/Documentation/DocData/build-info.json"))
    assert info["version"] == version and info["modules"] == ["image", "gallery"], "Wrong manual build or modules"
    assert {path for path in packaged_help if re.fullmatch(r"DocData/media-.+\.js", path)} == {"DocData/media-image.js", "DocData/media-gallery.js"}
    assert info["publicApiOnly"], "Full implementation sources must not be published"
    assert not any(re.search(r"\.(cpp|h|hpp|hlsl|ts|py|ps1|tgz)$", path) for path in packaged_help), "Source file distributed in manual"
    manifest = json.loads(package.read(f"{name}/Documentation/DocData/manifest.json"))
    assert [locale["code"] for locale in manifest["locales"]] == ["en", "fr"], "Manual must offer complete EN/FR only"
    assert not any(page.get("kind") == "source" for page in manifest["pages"]), "Source browser in manual"

payload = archive.read_bytes()
setup = installer.read_bytes()
assert setup[:2] == b"MZ", "Invalid installer stub"
assert setup[-16:-8] == b"CYIMGPK1", "Installer trailer missing"
length = struct.unpack("<Q", setup[-8:])[0]
assert length == len(payload), "Installer payload length mismatch"
assert setup[-16-length:-16] == payload, "Installer does not contain the verified ZIP"
print(f"Package checks: {version}; {len(packaged_help)} help files identical; {len(manifest['pages'])} pages; image + gallery; installer payload identical.")
print(f"ZIP SHA-256: {hashlib.sha256(payload).hexdigest()}")
