"""Fetch the engine's surface textures from Poly Haven and record where they came from.

Poly Haven publishes everything as CC0 and states that its assets are made
without generative AI (https://blog.polyhaven.com/ai-and-poly-haven/), which is
why it is the source. For each texture this downloads the 1k diffuse (albedo)
map, stores a 512x512 copy in data/textures/, and writes one manifest entry:
id, source URL, licence, authors, how it was made, and the fetch date.

scripts/build_textures.py refuses to pack any texture without a complete
manifest entry, so nothing reaches the engine with unknown provenance.

Run it only to add or refresh textures; the results are committed.
"""

import datetime
import io
import json
import pathlib
import sys
import urllib.request

from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parents[1]
OUT = ROOT / "data" / "textures"
SIZE = 512

# engine texture key -> Poly Haven asset id
TEXTURES = {
    "concrete": "brushed_concrete",
    "sheet_metal": "corrugated_iron",
    "girder": "rust_coarse_01",
    "wood": "brown_planks_05",
    "wood_dark": "dark_planks",
    "dirt": "dirt",
    "sand": "coast_sand_01",
    "grass": "sparse_grass",
    "snow": "snow_02",
    "asphalt": "asphalt_02",
}


# Poly Haven's API rejects anonymous clients; it asks for a descriptive agent.
HEADERS = {"User-Agent": "voxel-fps-engine-texture-fetch/1.0 (CC0 texture import)"}


def fetch(url: str, timeout: int = 60) -> bytes:
    with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=timeout) as r:
        return r.read()


def get_json(url: str):
    return json.loads(fetch(url).decode("utf-8"))


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = []
    today = datetime.date.today().isoformat()
    for key, asset in TEXTURES.items():
        info = get_json(f"https://api.polyhaven.com/info/{asset}")
        files = get_json(f"https://api.polyhaven.com/files/{asset}")
        diffuse = files.get("Diffuse") or files.get("diffuse")
        url = diffuse["1k"]["jpg"]["url"]
        img = Image.open(io.BytesIO(fetch(url, 120))).convert("RGB")
        img = img.resize((SIZE, SIZE), Image.LANCZOS)
        dst = OUT / f"{key}.jpg"
        img.save(dst, quality=92)
        manifest.append({
            "key": key,
            "file": dst.name,
            "source": "Poly Haven",
            "asset_id": asset,
            "source_url": f"https://polyhaven.com/a/{asset}",
            "download_url": url,
            "license": "CC0-1.0",
            "authors": sorted((info.get("authors") or {}).keys()),
            "creation_method": "photoscanned (Poly Haven: no generative AI)",
            "fetched": today,
        })
        print(f"{key:12s} <- {asset}")
    (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(manifest)} textures + manifest.json to {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
