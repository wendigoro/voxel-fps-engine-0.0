"""Validate data/textures/manifest.json and pack the textures for the engine.

The licence check the overhaul promised lives here and runs on every build:
  - every image in data/textures must have a manifest entry (no unvetted file
    can slip in beside the others),
  - every entry must name its source, source URL, licence, authors, creation
    method and fetch date,
  - the licence must be CC0-1.0,
  - the creation method must be photographic and must not be procedural or
    generative.
Any failure exits non-zero and the build stops.

Output: build/textures.bin, read by the engine (src/textures.hpp):
  "VTEX" | u32 version=1 | u32 layers | u32 size
  then per layer: 32-byte key (NUL padded) | size*size*4 bytes RGBA8
"""

import json
import pathlib
import struct
import sys

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
SRC = ROOT / "data" / "textures"
OUT = ROOT / "build" / "textures.bin"
SIZE = 512
REQUIRED = ("key", "file", "source", "source_url", "license", "authors", "creation_method", "fetched")
ALLOWED_LICENSES = {"CC0-1.0"}
REFUSED_METHODS = ("procedural", "generative", "ai-generated", "ai generated", "diffusion")


def main() -> int:
    manifest = json.loads((SRC / "manifest.json").read_text(encoding="utf-8"))
    problems = []
    listed = set()
    for i, e in enumerate(manifest):
        where = f"manifest entry {i} ({e.get('key', '?')})"
        for f in REQUIRED:
            if not e.get(f):
                problems.append(f"{where}: missing {f}")
        if e.get("license") not in ALLOWED_LICENSES:
            problems.append(f"{where}: licence {e.get('license')!r} is not one of {sorted(ALLOWED_LICENSES)}")
        method = str(e.get("creation_method", "")).lower()
        if "photo" not in method or any(w in method.replace("no generative", "") for w in REFUSED_METHODS):
            problems.append(f"{where}: creation method {e.get('creation_method')!r} is not photographic")
        if e.get("file") and not (SRC / e["file"]).exists():
            problems.append(f"{where}: file {e['file']} does not exist")
        listed.add(e.get("file"))
    for img in sorted(SRC.glob("*")):
        if img.suffix.lower() in (".jpg", ".jpeg", ".png") and img.name not in listed:
            problems.append(f"{img.name}: no manifest entry (unvetted texture)")
    keys = [e.get("key") for e in manifest]
    if len(keys) != len(set(keys)):
        problems.append("duplicate keys in manifest")
    if problems:
        print("build_textures: FAIL")
        for p in problems:
            print("  - " + p)
        return 1

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as f:
        f.write(b"VTEX" + struct.pack("<III", 1, len(manifest), SIZE))
        for e in manifest:
            img = Image.open(SRC / e["file"]).convert("RGBA")
            if img.size != (SIZE, SIZE):
                img = img.resize((SIZE, SIZE), Image.LANCZOS)
            f.write(e["key"].encode("ascii")[:31].ljust(32, b"\0"))
            f.write(img.tobytes())
    print(f"build_textures: OK ({len(manifest)} textures, all CC0 + photographic) -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
