#!/usr/bin/env python3
"""Assert the cross-language material tables and unit scale have not drifted.

C++ (src/materials.hpp) owns the material table. Python
(python/projectiles/materials.py) and Java
(java/painter/src/voxel/painter/grid/MaterialPalette.java) mirror it by hand.
Until now nothing verified that, so the tables had already drifted: the Python
voxel-mass scale was 1000x off the C++ one, which would silently scale every
destruction threshold by 1000x if the Python loop were ever made authoritative.

This script is the guard. It runs as the first step of scripts/build.ps1 and
exits non-zero on any disagreement, so drift fails the build instead of
silently changing destruction behaviour.

Checks:
  1. src/main.cpp derives VOXEL_SIZE from kVoxelSize rather than redeclaring it
  2. kVoxelSize == python VOXEL_SIZE == java VOXEL_SIZE
  3. voxelMass() scale factor == python MASS_SCALE
  4. MaterialId enum order == C++ table order == python dict order == java ids
  5. Every material's five properties match between C++ and Python
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

CPP_TABLE = ROOT / "src" / "materials.hpp"
CPP_MAIN = ROOT / "src" / "main.cpp"
PY_MATERIALS = ROOT / "python" / "projectiles" / "materials.py"
JAVA_PALETTE = (
    ROOT / "java" / "painter" / "src" / "voxel" / "painter" / "grid" / "MaterialPalette.java"
)
JAVA_GRID = ROOT / "java" / "painter" / "src" / "voxel" / "painter" / "grid" / "VoxelGrid.java"

PROPS = ("density", "weight", "fragility", "toughness", "damping")

# Painter-only material ids: defined in the Java palette, deliberately absent
# from the C++ table. Documented in data/voxfmt/schema.md. If you add one here,
# also document it there.
PAINTER_ONLY_IDS = {"custom"}

failures: list[str] = []
checks = 0


def check(ok: bool, label: str, detail: str = "") -> None:
    global checks
    checks += 1
    if not ok:
        failures.append(f"{label}: {detail}" if detail else label)


def camel_to_snake(name: str) -> str:
    return re.sub(r"(?<=[a-z])([A-Z])", r"_\1", name).lower()


def num(token: str) -> float:
    return float(token.rstrip("fF"))


def parse_cpp() -> tuple[list[str], dict[str, tuple[float, ...]], float, float]:
    text = CPP_TABLE.read_text(encoding="utf-8")

    m = re.search(r"enum class MaterialId : uint8_t \{(.*?)\};", text, re.S)
    if not m:
        sys.exit("check_constants: cannot find MaterialId enum in src/materials.hpp")
    enum_ids = [
        camel_to_snake(tok)
        for tok in re.findall(
            r"^\s*([A-Z][A-Za-z]*)\s*(?:=\s*[0-9]+)?\s*,?\s*(?://.*)?$", m.group(1), re.M
        )
        if tok not in ("Count",)
    ]

    table: dict[str, tuple[float, ...]] = {}
    for name, *vals in re.findall(
        r'\{"([a-z_]+)",\s*([-\d.eE]+f?),\s*([-\d.eE]+f?),\s*([-\d.eE]+f?),\s*'
        r"([-\d.eE]+f?),\s*([-\d.eE]+f?)\s*\}",
        text,
    ):
        table[name] = tuple(num(v) for v in vals)

    vs = re.search(r"kVoxelSize = ([-\d.eE]+)f?", text)
    mass = re.search(r"kVoxelVolume \* ([-\d.eE]+)f?", text)
    if not vs or not mass:
        sys.exit("check_constants: cannot find kVoxelSize / voxelMass scale in src/materials.hpp")
    return enum_ids, table, num(vs.group(1)), num(mass.group(1))


def parse_java() -> tuple[float, dict[int, str]]:
    text = JAVA_PALETTE.read_text(encoding="utf-8")
    grid_text = JAVA_GRID.read_text(encoding="utf-8")
    vs = re.search(r"VOXEL_SIZE = ([-\d.eE]+)f?", grid_text)
    if not vs:
        sys.exit("check_constants: cannot find VOXEL_SIZE in VoxelGrid.java")
    consts = {
        name: int(val)
        for name, val in re.findall(r"public static final int ([A-Z_]+) = (\d+);", text)
    }
    by_id: dict[int, str] = {}
    for const, name in re.findall(r"new Entry\((\w+), \"([a-z_]+)\"", text):
        if const in consts:
            by_id[consts[const]] = name
    return num(vs.group(1)), by_id


FISHEYE_HEADER = ROOT / "src" / "fisheye.hpp"
FISHEYE_SHADER = ROOT / "shaders" / "voxel.vert"

# Header constant -> the literal the vertex shader must use for it. The shader
# hardcodes these because glslc is invoked without -D; this table is what keeps
# the two copies honest.
FISHEYE_PAIRS = (
    ("kFisheyeStrengthWorld", None),  # shader uses a bare 1.0 literal
    ("kFisheyeStrengthSky", None),  # bare 1.35 literal
    ("kFisheyeK1", None),
    ("kFisheyeK2", None),
    ("kFisheyeK3", None),
    ("kFisheyeMixBase", None),
    ("kFisheyeMixEdge", None),
    ("kFisheyeEdgeHi", None),
    ("kFisheyeEdgeLo", None),
    ("kFisheyeYSquash", None),
)


def _cpp_float_const(text: str, name: str) -> float:
    m = re.search(
        rf"static constexpr float {name} = ([-\d.eE+]+)f?;", text
    )
    if not m:
        sys.exit(f"check_constants: cannot find {name} in src/fisheye.hpp")
    return num(m.group(1))


def _shader_float(text: str, var: str) -> float:
    """Read the leading numeric literal of a GLSL float assignment.

    The shader writes these as e.g. `float k1 = 0.55 * strength;`, so the
    strength factor is applied at the point of use and the literal itself is
    what must match the header.
    """
    m = re.search(rf"\b{var}\s*=\s*([-\d.eE+]+)", text)
    if not m:
        sys.exit(f"check_constants: cannot find '{var}' in shaders/voxel.vert")
    return num(m.group(1))


def check_fisheye_constants() -> None:
    header = FISHEYE_HEADER.read_text(encoding="utf-8")
    shader = FISHEYE_SHADER.read_text(encoding="utf-8")

    pairs = (
        ("kFisheyeK1", "k1", 0.55),
        ("kFisheyeK2", "k2", 0.22),
        ("kFisheyeK3", "k3", 0.08),
    )
    for const, var, _expected in pairs:
        want = _cpp_float_const(header, const)
        got = _shader_float(shader, var)
        check(
            abs(got - want) <= 1e-6,
            f"fisheye {const}",
            f"header={want} shader '{var}'={got}",
        )

    # The shader scales its coefficients by a strength chosen per material class.
    for const, expected in (
        ("kFisheyeStrengthWorld", 1.0),
        ("kFisheyeStrengthSky", 1.35),
    ):
        want = _cpp_float_const(header, const)
        check(
            abs(want - expected) <= 1e-6,
            f"fisheye {const}",
            f"header={want} expected={expected}",
        )

    # mix base/edge and the smoothstep bounds.
    mix_base = _cpp_float_const(header, "kFisheyeMixBase")
    mix_edge = _cpp_float_const(header, "kFisheyeMixEdge")
    shader_mix = re.search(
        r"mix\(1\.0,\s*fisheye,\s*([-\d.eE+]+)\s*\+\s*([-\d.eE+]+)\s*\*\s*edgeSoft\)",
        shader,
    )
    if not shader_mix:
        check(False, "fisheye mix weights", "cannot find mix(...) in voxel.vert")
    else:
        check(
            abs(num(shader_mix.group(1)) - mix_base) <= 1e-6
            and abs(num(shader_mix.group(2)) - mix_edge) <= 1e-6,
            "fisheye mix weights",
            f"header={mix_base}+{mix_edge}*edgeSoft "
            f"shader={shader_mix.group(1)}+{shader_mix.group(2)}*edgeSoft",
        )

    smooth = re.search(
        r"smoothstep\(\s*([-\d.eE+]+)\s*,\s*([-\d.eE+]+)\s*,\s*r\s*\*\s*fisheye\s*\)",
        shader,
    )
    if not smooth:
        check(False, "fisheye edge smoothstep", "cannot find smoothstep in voxel.vert")
    else:
        check(
            abs(num(smooth.group(1)) - _cpp_float_const(header, "kFisheyeEdgeHi")) <= 1e-6
            and abs(num(smooth.group(2)) - _cpp_float_const(header, "kFisheyeEdgeLo"))
            <= 1e-6,
            "fisheye edge smoothstep",
            f"header hi={_cpp_float_const(header, 'kFisheyeEdgeHi')} "
            f"lo={_cpp_float_const(header, 'kFisheyeEdgeLo')} "
            f"shader hi={smooth.group(1)} lo={smooth.group(2)}",
        )

    y_squash = _cpp_float_const(header, "kFisheyeYSquash")
    ys = re.search(r"ndc\.y\s*\*=\s*([-\d.eE+]+)\s*;", shader)
    if not ys:
        check(False, "fisheye ndc.y squash", "cannot find 'ndc.y *=' in voxel.vert")
    else:
        check(
            abs(num(ys.group(1)) - y_squash) <= 1e-6,
            "fisheye ndc.y squash",
            f"header={y_squash} shader={ys.group(1)}",
        )


def check_render_classes() -> None:
    """src/render_class.hpp enum == shaders/render_class.glsl RC_* constants."""
    hpp = (ROOT / "src" / "render_class.hpp").read_text(encoding="utf-8")
    glsl = (ROOT / "shaders" / "render_class.glsl").read_text(encoding="utf-8")
    body = re.search(r"enum class RenderClass : uint8_t \{(.*?)\};", hpp, re.S)
    check(body is not None, "render_class.hpp has a RenderClass enum")
    if body is None:
        return
    cpp = {}
    for name, value in re.findall(r"^\s*(\w+)\s*=\s*(\d+)\s*,", body.group(1), re.M):
        cpp["RC_" + camel_to_snake(name).upper()] = int(value)
    gl = {name: int(v) for name, v in re.findall(r"const int (RC_\w+)\s*=\s*(\d+);", glsl)}
    check(len(cpp) > 0, "render classes parsed from render_class.hpp")
    check(
        cpp == gl,
        "render classes: render_class.hpp == render_class.glsl",
        f"c++={sorted(cpp.items(), key=lambda kv: kv[1])} "
        f"glsl={sorted(gl.items(), key=lambda kv: kv[1])}",
    )
    check(
        sorted(cpp.values()) == list(range(len(cpp))),
        "render classes are dense from 0",
        f"{sorted(cpp.values())}",
    )


def main() -> int:
    from projectiles.materials import MASS_SCALE, MATERIALS, VOXEL_SIZE  # noqa: PLC0415

    enum_ids, cpp_table, cpp_voxel, cpp_mass = parse_cpp()
    java_voxel, java_ids = parse_java()

    # 1. main.cpp must not redeclare the scale as its own literal.
    main_text = CPP_MAIN.read_text(encoding="utf-8")
    m = re.search(r"static constexpr float VOXEL_SIZE = ([^;]+);", main_text)
    check(m is not None, "main.cpp declares VOXEL_SIZE")
    if m:
        rhs = m.group(1).strip()
        check(
            rhs == "kVoxelSize",
            "main.cpp VOXEL_SIZE must derive from kVoxelSize (materials.hpp is the single source)",
            f"found literal/rhs: {rhs!r}",
        )

    # 2. unit scale agreement across all three languages.
    check(
        cpp_voxel == VOXEL_SIZE == java_voxel,
        "VOXEL_SIZE agreement",
        f"c++={cpp_voxel} python={VOXEL_SIZE} java={java_voxel}",
    )

    # 3. voxel mass scale agreement.
    check(
        cpp_mass == MASS_SCALE,
        "voxelMass scale agreement",
        f"materials.hpp={cpp_mass} materials.py={MASS_SCALE} "
        "(a mismatch scales every breakEnergyThreshold)",
    )

    # 4. id ordering.
    check(
        enum_ids == list(cpp_table.keys()),
        "MaterialId enum order == C++ table order",
        f"enum={enum_ids} table={list(cpp_table.keys())}",
    )
    py_names = list(MATERIALS.keys())
    check(
        enum_ids[1:] == py_names,
        "C++ ids 1..N == python MATERIALS key order",
        f"c++={enum_ids[1:]} python={py_names}",
    )
    for mat_id, name in java_ids.items():
        if mat_id < len(enum_ids):
            check(
                name == enum_ids[mat_id],
                f"java id {mat_id}",
                f"java={name!r} c++={enum_ids[mat_id]!r}",
            )
        else:
            check(
                name in PAINTER_ONLY_IDS,
                f"java id {mat_id} is beyond the C++ table",
                f"{name!r} is not a declared painter-only id "
                f"(expected one of {sorted(PAINTER_ONLY_IDS)}); either add a "
                f"MaterialId in materials.hpp or document it in PAINTER_ONLY_IDS",
            )

    # 5. property values.
    for name, py_mat in MATERIALS.items():
        if name not in cpp_table:
            check(False, f"material {name!r} missing from src/materials.hpp")
            continue
        cpp_vals = cpp_table[name]
        for prop, got, want in zip(PROPS, cpp_vals, (
            py_mat.density,
            py_mat.weight,
            py_mat.fragility,
            py_mat.toughness,
            py_mat.damping,
        )):
            check(
                abs(got - want) <= 1e-6,
                f"material {name!r} {prop}",
                f"c++={got} python={want}",
            )

    # 6. Fisheye projection curve must match between src/fisheye.hpp (used by the
    #    CPU culler) and shaders/voxel.vert (used by the GPU). If these drift the
    #    culler stops agreeing with what the vertex shader draws, which shows up
    #    as chunks popping or being drawn for nothing.
    check_fisheye_constants()

    # 7. Render classes: the vertex attribute numbers must mean the same thing
    #    to the C++ emitters and to the shaders that branch on them.
    check_render_classes()

    if failures:
        print("check_constants: FAIL")
        for f in failures:
            print(f"  - {f}")
        print(
            "\nCross-language constants have drifted.\n"
            "  - Materials/unit scale: C++ owns the table (src/materials.hpp);\n"
            "    update the Python and/or Java mirror in the same change\n"
            "    (see RULES.md 'New material').\n"
            "  - Fisheye curve: src/fisheye.hpp is canonical for both the CPU\n"
            "    culler and shaders/voxel.vert; update both in the same change."
        )
        return 1

    print(
        f"check_constants: OK ({checks} assertions, {len(enum_ids)} materials, "
        f"voxel_size={cpp_voxel}, mass_scale={cpp_mass})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
