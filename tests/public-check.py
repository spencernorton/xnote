#!/usr/bin/env python3
"""Fast, dependency-free checks for XNote's public release surface."""

from pathlib import Path
import re
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
VERSION = "3.3.0"
GITHUB = "https://github.com/spencernorton/xnote"


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"public-check: {message}")


configure = read("configure.ac")
match = re.search(r"AC_INIT\(\[XNote\],\[([^]]+)\]", configure)
require(match is not None and match.group(1) == VERSION,
        f"configure.ac must declare XNote {VERSION}")
require(f"[{GITHUB}/issues]" in configure,
        "configure.ac must use the public issue tracker")

appdata_path = ROOT / "data/xnote.appdata.xml.in"
appdata = ET.parse(appdata_path).getroot()
require(appdata.findtext("id") == "tech.norvi.xnote",
        "AppStream id changed unexpectedly")
require(appdata.findtext("project_license") == "GPL-3.0-or-later",
        "AppStream project license must be GPL-3.0-or-later")
require(any(node.get("version") == VERSION for node in appdata.findall("./releases/release")),
        f"AppStream releases must contain {VERSION}")
require(appdata.find("update_contact") is None,
        "AppStream metadata must not publish a personal contact address")

icon_path = ROOT / "images/hicolor/scalable/apps/xnote.svg"
icon = ET.parse(icon_path).getroot()
require(icon.tag.endswith("svg"), "application icon must be a valid SVG")
require(not list((ROOT / "images/hicolor").glob("*x*/apps/xnote.png")),
        "undocumented raster application icons must not be published")

go_mod = read("cloud-helper/go.mod")
require(go_mod.startswith(
    "module github.com/spencercnorton/xnote/cloud-helper\n"),
    "cloud-helper module path must be public")
require("\ngo 1.26.8\n" in go_mod,
        "cloud-helper must reject toolchains affected by GO-2026-4869")
require("637c7b51f1b09a28553a926f594f626d363c526a" in read("NOTICE"),
        "NOTICE must identify the audited upstream baseline")
require("GNU GENERAL PUBLIC LICENSE" in read("COPYING"),
        "COPYING must contain the GPL text")
require("GNU LESSER GENERAL PUBLIC LICENSE" in read("src/COPYING.LESSER"),
        "src/COPYING.LESSER must contain the LGPL text")
# GitHub's licence detection reads only the repository root: a second licence
# file there (COPYING.LESSER beside COPYING) makes it label the project LGPL.
require(not (ROOT / "COPYING.LESSER").exists(),
        "the LGPL text belongs in src/, not beside COPYING")
require(not (ROOT / "cloud-helper/xnote-cloud-backup").exists(),
        "compiled cloud-helper binary must not be in the release tree")
require(not (ROOT / "autopackage").exists(),
        "obsolete Autopackage metadata must not be in the release tree")
require("-mod=vendor" in read("Makefile.am"),
        "Go builds must use the checksummed vendored modules")
require("GOTOOLCHAIN=local" in read("Makefile.am"),
        "Go builds must reject older local toolchains instead of downloading one")
require("GOPROXY=off" in read("Makefile.am"),
        "Go builds must remain offline")
for module in ("crypto", "sys", "term"):
    module_root = ROOT / "cloud-helper/vendor/golang.org/x" / module
    require((module_root / "LICENSE").is_file(),
            f"vendored golang.org/x/{module} must retain LICENSE")
    require((module_root / "PATENTS").is_file(),
            f"vendored golang.org/x/{module} must retain PATENTS")
require((ROOT / "cloud-helper/vendor/modules.txt").is_file(),
        "vendored module manifest must be published")
for path in (
    "README.md",
    "SECURITY.md",
    "CHANGELOG.md",
    "configure.ac",
    "data/xnote.appdata.xml.in",
    "doc/xnote.1",
    "doc/xnote-user-help.txt",
    "src/xpad-pad.c",
    "cloud-helper/go.mod",
    "debian/control",
    "scripts/build-deb.sh",
):
    read(path)

require(GITHUB in read("README.md"), "README must point to the public repository")
print("public-check: PASS")
