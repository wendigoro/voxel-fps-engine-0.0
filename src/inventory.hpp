#pragma once
// Cubic-unit inventory (C++ side). Implements RULES.md rule 12.
//
// Items are 1x1x1 unit-cube lattices on the same integer grid as the world
// (kVoxelSize == 0.001). Inventory volume is a SEPARATE occupancy layer: it is
// never written into chunk storage (Block) and never affects player collision,
// raycast impact, destruction, or water sampling.
//
// Display-layer exception: when the inventory is open, its unit cubes are
// composited over map voxels in a dedicated overlay render pass with a cleared
// depth buffer. This is the only sanctioned way to draw non-map voxels over
// map voxels. Lattice packing and rotation operate on integer unit cells only.

#include "materials.hpp"
#include "destruction.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// ---- item classes --------------------------------------------------------

enum class ItemClass : uint8_t {
    WeaponPrimary = 0, // fits Primary0 / Primary1
    WeaponSmall,       // fits Small
    Armor,             // must fit entirely inside one armor zone
    Backpack,          // fits Backpack slot; grants the storage volume
    AmmoPouch,         // reserve ammo carrier
    Misc,
};

inline const char* itemClassName(ItemClass c) {
    switch (c) {
    case ItemClass::WeaponPrimary: return "weapon_primary";
    case ItemClass::WeaponSmall: return "weapon_small";
    case ItemClass::Armor: return "armor";
    case ItemClass::Backpack: return "backpack";
    case ItemClass::AmmoPouch: return "ammo_pouch";
    default: return "misc";
    }
}

inline ItemClass itemClassFromName(const std::string& s) {
    if (s == "weapon_primary") return ItemClass::WeaponPrimary;
    if (s == "weapon_small") return ItemClass::WeaponSmall;
    if (s == "armor") return ItemClass::Armor;
    if (s == "backpack") return ItemClass::Backpack;
    if (s == "ammo_pouch") return ItemClass::AmmoPouch;
    return ItemClass::Misc;
}

// ---- armor zones ---------------------------------------------------------

// Default character hitbox in integer unit cells, derived from PlayerBody
// (radius 0.0022 = 2.2 cells half-extent, height 0.0185 = 18.5 cells) exactly
// as playerHitsSolid() iterates it: x/z in [-2, +2], y in [0, 18].
inline constexpr int kHitHalfXZ = 2;
inline constexpr int kHitHeight = 19;
inline constexpr int kHitCells =
    (2 * kHitHalfXZ + 1) * (2 * kHitHalfXZ + 1) * kHitHeight; // 475

enum class ArmorZone : uint8_t { Legs = 0, Chest, Arms, Head, Count };

// An inclusive axis-aligned box of unit cells, in hitbox cell coordinates
// (x/z relative to the body centre column, y measured up from the feet).
struct ZoneBox {
    int x0, y0, z0;
    int x1, y1, z1;

    int cellCount() const { return (x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1); }
    bool contains(int x, int y, int z) const {
        return x >= x0 && x <= x1 && y >= y0 && y <= y1 && z >= z0 && z <= z1;
    }
};

struct ArmorZoneDef {
    ArmorZone zone;
    const char* name;
    std::vector<ZoneBox> boxes;
};

// The four armor zones tile the 5x19x5 hitbox exactly: no dead cells, no
// overlaps. Legs/Chest/Head are single boxes; Arms is two separate side
// columns, which is why zones are a list of boxes rather than one predicate.
inline const std::vector<ArmorZoneDef>& armorZoneTable() {
    static const std::vector<ArmorZoneDef> kZones = {
        {ArmorZone::Legs, "legs", {{-2, 0, -2, 2, 8, 2}}},
        {ArmorZone::Chest, "chest", {{-1, 9, -2, 1, 15, 2}}},
        {ArmorZone::Arms, "arms", {{-2, 9, -2, -2, 15, 2}, {2, 9, -2, 2, 15, 2}}},
        {ArmorZone::Head, "head", {{-2, 16, -2, 2, 18, 2}}},
    };
    return kZones;
}

inline const ArmorZoneDef& armorZoneDef(ArmorZone z) {
    const auto& t = armorZoneTable();
    const size_t i = static_cast<size_t>(z);
    if (i >= t.size()) return t[0];
    return t[i];
}

inline const char* armorZoneName(ArmorZone z) {
    const size_t i = static_cast<size_t>(z);
    if (i >= armorZoneTable().size()) return "none";
    return armorZoneTable()[i].name;
}

// -1 for an unset/unknown zone, which means "fits any zone it physically fits".
inline int armorZoneFromName(const std::string& s) {
    const auto& t = armorZoneTable();
    for (size_t i = 0; i < t.size(); ++i) {
        if (s == t[i].name) return static_cast<int>(i);
    }
    return -1;
}

// Zone owning a hitbox cell. Every in-bounds cell resolves to exactly one zone.
inline ArmorZone armorZoneAt(int x, int y, int z) {
    for (const auto& zd : armorZoneTable()) {
        for (const auto& b : zd.boxes) {
            if (b.contains(x, y, z)) return zd.zone;
        }
    }
    return ArmorZone::Count;
}

inline bool hitCellInBounds(int x, int y, int z) {
    return x >= -kHitHalfXZ && x <= kHitHalfXZ && y >= 0 && y < kHitHeight &&
           z >= -kHitHalfXZ && z <= kHitHalfXZ;
}

// Invariant guard: zones cover every hitbox cell exactly once. Cheap enough to
// assert from the smoke run.
inline bool armorZonesTileHitbox() {
    std::vector<uint8_t> cover(kHitCells, 0);
    for (int y = 0; y < kHitHeight; ++y) {
        for (int z = -kHitHalfXZ; z <= kHitHalfXZ; ++z) {
            for (int x = -kHitHalfXZ; x <= kHitHalfXZ; ++x) {
                if (armorZoneAt(x, y, z) == ArmorZone::Count) return false;
                const int c = (x + kHitHalfXZ) + y * (2 * kHitHalfXZ + 1) +
                              (z + kHitHalfXZ) * (2 * kHitHalfXZ + 1) * kHitHeight;
                if (++cover[c] != 1) return false; // overlap
            }
        }
    }
    for (uint8_t v : cover) {
        if (v != 1) return false;
    }
    return true;
}

// ---- item shape: a lattice of unit cells --------------------------------

// Occupancy lattice of 1x1x1 unit cubes, indexed [ix + iy*sx + iz*sx*sy].
// A non-zero cell is a solid cube; a zero cell is a gap the item does not fill.
struct ItemShape {
    int sx = 0, sy = 0, sz = 0;
    std::vector<uint8_t> cells;

    int cellCount() const { return static_cast<int>(cells.size()); }

    int solidCount() const {
        int n = 0;
        for (uint8_t c : cells) {
            if (c) ++n;
        }
        return n;
    }

    bool valid() const {
        if (sx <= 0 || sy <= 0 || sz <= 0) return false;
        return cells.size() == static_cast<size_t>(sx) * static_cast<size_t>(sy) *
                                   static_cast<size_t>(sz);
    }

    bool inBounds(int ix, int iy, int iz) const {
        return ix >= 0 && iy >= 0 && iz >= 0 && ix < sx && iy < sy && iz < sz;
    }

    int index(int ix, int iy, int iz) const { return ix + iy * sx + iz * sx * sy; }

    bool solid(int ix, int iy, int iz) const {
        if (!inBounds(ix, iy, iz)) return false;
        return cells[static_cast<size_t>(index(ix, iy, iz))] != 0;
    }

    void set(int ix, int iy, int iz, bool v) {
        if (!inBounds(ix, iy, iz)) return;
        cells[static_cast<size_t>(index(ix, iy, iz))] = v ? 1 : 0;
    }
};

inline ItemShape makeBoxShape(int sx, int sy, int sz) {
    ItemShape s;
    if (sx <= 0 || sy <= 0 || sz <= 0) return s;
    s.sx = sx;
    s.sy = sy;
    s.sz = sz;
    s.cells.assign(static_cast<size_t>(sx) * static_cast<size_t>(sy) * static_cast<size_t>(sz), 1);
    return s;
}

// Build a shape from a flat list of (x,y,z) cell triples, in any order.
inline ItemShape makeShapeFromCells(int sx, int sy, int sz, const std::vector<int>& triples) {
    ItemShape s;
    if (sx <= 0 || sy <= 0 || sz <= 0) return s;
    s.sx = sx;
    s.sy = sy;
    s.sz = sz;
    s.cells.assign(static_cast<size_t>(sx) * static_cast<size_t>(sy) * static_cast<size_t>(sz), 0);
    for (size_t i = 0; i + 2 < triples.size(); i += 3) {
        s.set(triples[i], triples[i + 1], triples[i + 2], true);
    }
    return s;
}

// 90 degree rotation about the Y axis. Cells stay axis-aligned 1x1x1 unit cubes
// on the integer grid: rule 12 permits lattice rotation, never rescaling.
inline ItemShape rotateShapeY90(const ItemShape& in) {
    if (!in.valid()) return in;
    ItemShape out;
    out.sx = in.sz;
    out.sy = in.sy;
    out.sz = in.sx;
    out.cells.assign(in.cells.size(), 0);
    for (int iz = 0; iz < in.sz; ++iz) {
        for (int iy = 0; iy < in.sy; ++iy) {
            for (int ix = 0; ix < in.sx; ++ix) {
                if (!in.solid(ix, iy, iz)) continue;
                out.set(in.sz - 1 - iz, iy, ix, true);
            }
        }
    }
    return out;
}

inline ItemShape rotatedShape(const ItemShape& in, int rot) {
    ItemShape s = in;
    for (int i = 0; i < (rot & 3); ++i) s = rotateShapeY90(s);
    return s;
}

// ---- item definitions ----------------------------------------------------

struct ItemDef {
    std::string id;
    std::string name;
    ItemClass cls = ItemClass::Misc;
    ItemShape shape;
    MaterialId material = MaterialId::SheetMetal;
    float cr = 0.7f, cg = 0.7f, cb = 0.7f; // display tint (mat 7 branch)

    // Weapon payload
    std::string weaponId;    // WeaponDef id for weapon classes
    std::string ammoId;      // chambered ammo subtype
    int magazineSize = 0;    // rounds per magazine

    // Ammo pouch payload
    std::string caliber;     // ammo subtype this pouch holds
    int rounds = 0;          // starting / max rounds

    // Backpack payload: granted storage volume in unit cells (0 = not a pack)
    int packSX = 0, packSY = 0, packSZ = 0;

    // Armor: zone this piece must fit, -1 = any zone
    int armorZone = -1;

    // RULES.md rule 12 authoring contract (unit grid only).
    int unit = 1;
    float voxelSize = 0.001f;
};

using ItemTable = std::vector<ItemDef>;

inline int itemVolumeCells(const ItemDef& d) {
    return d.shape.valid() ? d.shape.solidCount() : 0;
}

inline const ItemDef* findItemById(const ItemTable& t, const std::string& id) {
    for (const auto& d : t) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

inline const ItemDef* itemDefAt(const ItemTable& t, int index) {
    if (index < 0 || index >= static_cast<int>(t.size())) return nullptr;
    return &t[static_cast<size_t>(index)];
}

// Contract check: items must be authored at unit=1, voxel_size=0.001, on a
// cubic unit lattice. Anything else is rejected at load time so the painter and
// the engine cannot drift apart.
inline bool itemGridValid(const ItemDef& d) {
    if (d.unit != 1) return false;
    if (std::fabs(d.voxelSize - 0.001f) > 1e-6f) return false;
    if (!d.shape.valid()) return false;
    if (d.shape.solidCount() <= 0) return false;
    return true;
}

// ---- equipment slots -----------------------------------------------------

enum class EquipSlot : uint8_t {
    Primary0 = 0,
    Primary1,
    Small,
    ArmorLegs,
    ArmorChest,
    ArmorArms,
    ArmorHead,
    Backpack,
    Count
};

inline constexpr int kEquipSlotCount = static_cast<int>(EquipSlot::Count); // 8

inline const char* equipSlotName(EquipSlot s) {
    switch (s) {
    case EquipSlot::Primary0: return "primary_0";
    case EquipSlot::Primary1: return "primary_1";
    case EquipSlot::Small: return "small";
    case EquipSlot::ArmorLegs: return "armor_legs";
    case EquipSlot::ArmorChest: return "armor_chest";
    case EquipSlot::ArmorArms: return "armor_arms";
    case EquipSlot::ArmorHead: return "armor_head";
    case EquipSlot::Backpack: return "backpack";
    default: return "none";
    }
}

inline bool slotIsArmor(EquipSlot s) {
    return s == EquipSlot::ArmorLegs || s == EquipSlot::ArmorChest ||
           s == EquipSlot::ArmorArms || s == EquipSlot::ArmorHead;
}

inline bool slotIsWeapon(EquipSlot s) {
    return s == EquipSlot::Primary0 || s == EquipSlot::Primary1 || s == EquipSlot::Small;
}

// Armor slot -> hitbox zone, or Count when the slot is not an armor slot.
inline ArmorZone equipSlotZone(EquipSlot s) {
    switch (s) {
    case EquipSlot::ArmorLegs: return ArmorZone::Legs;
    case EquipSlot::ArmorChest: return ArmorZone::Chest;
    case EquipSlot::ArmorArms: return ArmorZone::Arms;
    case EquipSlot::ArmorHead: return ArmorZone::Head;
    default: return ArmorZone::Count;
    }
}

inline EquipSlot zoneEquipSlot(ArmorZone z) {
    switch (z) {
    case ArmorZone::Legs: return EquipSlot::ArmorLegs;
    case ArmorZone::Chest: return EquipSlot::ArmorChest;
    case ArmorZone::Arms: return EquipSlot::ArmorArms;
    case ArmorZone::Head: return EquipSlot::ArmorHead;
    default: return EquipSlot::Count;
    }
}

inline bool slotAcceptsClass(EquipSlot s, ItemClass c) {
    switch (s) {
    case EquipSlot::Primary0:
    case EquipSlot::Primary1:
        return c == ItemClass::WeaponPrimary;
    case EquipSlot::Small:
        return c == ItemClass::WeaponSmall;
    case EquipSlot::Backpack:
        return c == ItemClass::Backpack;
    default:
        return slotIsArmor(s) && c == ItemClass::Armor;
    }
}

// ---- storage volume ------------------------------------------------------

// Base backpack interior: 3x3x4 = 36 unit cells. Two 2x2x2 ammo pouches (8
// cells each) fit inside, leaving 20 free.
inline constexpr int kBasePackSX = 3;
inline constexpr int kBasePackSY = 3;
inline constexpr int kBasePackSZ = 4;
inline constexpr int kBasePackCells = kBasePackSX * kBasePackSY * kBasePackSZ; // 36

// Reserve ammo carrier: 2x2x2 = 8 unit cells.
inline constexpr int kAmmoPouchEdge = 2;
inline constexpr int kAmmoPouchCells = kAmmoPouchEdge * kAmmoPouchEdge * kAmmoPouchEdge;

struct PackVolume {
    int sx = kBasePackSX, sy = kBasePackSY, sz = kBasePackSZ;

    // No storage at all (backpack unequipped, or a malformed pack def). Needed
    // as an explicit factory: a default-constructed PackVolume is the 3x3x4
    // BASE pack, so "clear it" must not be spelled PackVolume{}.
    static PackVolume none() {
        PackVolume v;
        v.sx = v.sy = v.sz = 0;
        return v;
    }

    int cells() const { return sx * sy * sz; }
    bool valid() const { return sx > 0 && sy > 0 && sz > 0; }
    int index(int x, int y, int z) const { return x + y * sx + z * sx * sy; }
    bool inBounds(int x, int y, int z) const {
        return x >= 0 && y >= 0 && z >= 0 && x < sx && y < sy && z < sz;
    }
};

// ---- placed instances ----------------------------------------------------

struct ItemInstance {
    int defIndex = -1;               // index into the item def table
    int ox = 0, oy = 0, oz = 0;      // origin (min corner) in lattice cells
    int rot = 0;                     // quarter turns about Y, 0..3
    int stack = 1;                   // quantity for stackable items
    int rounds = 0;                  // live rounds (weapons + pouches)
};

// ---- inventory -----------------------------------------------------------

struct Inventory {
    // Equipment: -1 = empty, else index into the item def table.
    int slotDef[kEquipSlotCount];
    int slotRot[kEquipSlotCount];

    PackVolume vol;                  // granted by the equipped backpack
    std::vector<int> occ;            // per cell: -1 empty, else instance index
    std::vector<ItemInstance> items; // instances packed in the volume

    int activeWeaponSlot = 0;        // equip slot providing the held weapon
    int activePouch = -1;            // instance index feeding the reserve
    int held = -1;                   // instance currently in hand (-1 = none)

    Inventory() { resetSlots(); }

    void resetSlots() {
        for (int i = 0; i < kEquipSlotCount; ++i) {
            slotDef[i] = -1;
            slotRot[i] = 0;
        }
    }

    int usedCells() const {
        int n = 0;
        for (int o : occ) {
            if (o >= 0) ++n;
        }
        return n;
    }

    int freeCells() const { return static_cast<int>(occ.size()) - usedCells(); }

    void setVolume(const PackVolume& v) {
        vol = v;
        occ.assign(static_cast<size_t>(v.cells()), -1);
        items.clear();
        // items was cleared, so a hand index would dangle.
        held = -1;
    }
};

// ---- packing -------------------------------------------------------------

// Can `shape` sit at lattice origin (ox,oy,oz) without collision or overflow?
inline bool packFits(const Inventory& inv, const ItemShape& shape, int ox, int oy, int oz) {
    if (!shape.valid()) return false;
    if (ox < 0 || oy < 0 || oz < 0) return false;
    if (ox + shape.sx > inv.vol.sx) return false;
    if (oy + shape.sy > inv.vol.sy) return false;
    if (oz + shape.sz > inv.vol.sz) return false;
    for (int iz = 0; iz < shape.sz; ++iz) {
        for (int iy = 0; iy < shape.sy; ++iy) {
            for (int ix = 0; ix < shape.sx; ++ix) {
                if (!shape.solid(ix, iy, iz)) continue;
                if (inv.occ[static_cast<size_t>(inv.vol.index(ox + ix, oy + iy, oz + iz))] >= 0)
                    return false;
            }
        }
    }
    return true;
}

// Auto-place: first fit over a bottom-up origin scan, trying all four Y
// rotations. Sets outInst to the new instance index and outRot to the rotation
// it settled on. Returns false when the item does not fit at all.
inline bool autoPlace(Inventory& inv, const ItemTable& table, int defIndex,
                      int& outInst, int& outRot) {
    const ItemDef* def = itemDefAt(table, defIndex);
    if (!def || !def->shape.valid()) return false;
    if (inv.items.size() >= 255) return false;

    for (int rot = 0; rot < 4; ++rot) {
        ItemShape s = rotatedShape(def->shape, rot);
        for (int oz = 0; oz + s.sz <= inv.vol.sz; ++oz) {
            for (int oy = 0; oy + s.sy <= inv.vol.sy; ++oy) {
                for (int ox = 0; ox + s.sx <= inv.vol.sx; ++ox) {
                    if (!packFits(inv, s, ox, oy, oz)) continue;

                    ItemInstance inst;
                    inst.defIndex = defIndex;
                    inst.ox = ox;
                    inst.oy = oy;
                    inst.oz = oz;
                    inst.rot = rot;
                    inst.rounds = def->cls == ItemClass::AmmoPouch
                                      ? def->rounds
                                      : (def->cls == ItemClass::WeaponPrimary ||
                                                 def->cls == ItemClass::WeaponSmall
                                             ? def->magazineSize
                                             : 0);
                    const int idx = static_cast<int>(inv.items.size());
                    inv.items.push_back(inst);
                    for (int iz = 0; iz < s.sz; ++iz) {
                        for (int iy = 0; iy < s.sy; ++iy) {
                            for (int ix = 0; ix < s.sx; ++ix) {
                                if (!s.solid(ix, iy, iz)) continue;
                                inv.occ[static_cast<size_t>(
                                    inv.vol.index(ox + ix, oy + iy, oz + iz))] = idx;
                            }
                        }
                    }
                    outInst = idx;
                    outRot = rot;
                    return true;
                }
            }
        }
    }
    return false;
}

// Remove a packed instance and free its cells. Every stored instance index is
// fixed up, including the hand and the active pouch, so a removal can never
// leave a dangling index behind.
inline bool removePacked(Inventory& inv, int instIndex) {
    if (instIndex < 0 || instIndex >= static_cast<int>(inv.items.size())) return false;
    inv.items.erase(inv.items.begin() + instIndex);
    for (int& o : inv.occ) {
        if (o == instIndex) o = -1;
        else if (o > instIndex) --o;
    }
    if (inv.held == instIndex) inv.held = -1;
    else if (inv.held > instIndex) --inv.held;
    if (inv.activePouch == instIndex) inv.activePouch = -1;
    else if (inv.activePouch > instIndex) --inv.activePouch;
    return true;
}

// ---- hand: lift / place / stow (look-and-click movement) ------------------
//
// These back the look-and-click interaction. All of them move the SAME instance
// rather than removing and re-adding it, so a hand-carried item keeps its
// identity (and its live round count) across a move. Occupancy is only ever
// rewritten through placeHeld, and placement is validated by packFits first, so
// a move can never leave occ pointing at cells the shape does not cover.

// Lift a packed instance into the hand. Its cells become free, but the instance
// stays in the list so no other index shifts.
inline bool liftPacked(Inventory& inv, int instIndex) {
    if (inv.held >= 0) return false; // one item in hand at a time
    if (instIndex < 0 || instIndex >= static_cast<int>(inv.items.size())) return false;
    for (int& o : inv.occ) {
        if (o == instIndex) o = -1;
    }
    inv.held = instIndex;
    return true;
}

// Instance index packed into a lattice cell, or -1 if the cell is empty or the
// coords fall outside the volume.
inline int invIndexAt(const Inventory& inv, int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0) return -1;
    if (x >= inv.vol.sx || y >= inv.vol.sy || z >= inv.vol.sz) return -1;
    return inv.occ[static_cast<size_t>(inv.vol.index(x, y, z))];
}

inline bool handFits(const Inventory& inv, const ItemTable& table, int rot, int ox, int oy, int oz) {
    if (inv.held < 0 || inv.held >= static_cast<int>(inv.items.size())) return false;
    const ItemDef* d = itemDefAt(table, inv.items[static_cast<size_t>(inv.held)].defIndex);
    if (!d) return false;
    // The hand's own cells are already cleared, so packFits cannot self-collide.
    return packFits(inv, rotatedShape(d->shape, rot), ox, oy, oz);
}

inline bool placeHeld(Inventory& inv, const ItemTable& table, int rot, int ox, int oy, int oz) {
    if (!handFits(inv, table, rot, ox, oy, oz)) return false;
    const int inst = inv.held;
    ItemInstance& it = inv.items[static_cast<size_t>(inst)];
    it.rot = rot;
    it.ox = ox;
    it.oy = oy;
    it.oz = oz;
    const ItemDef* d = itemDefAt(table, it.defIndex);
    const ItemShape s = rotatedShape(d->shape, rot);
    for (int iz = 0; iz < s.sz; ++iz) {
        for (int iy = 0; iy < s.sy; ++iy) {
            for (int ix = 0; ix < s.sx; ++ix) {
                if (!s.solid(ix, iy, iz)) continue;
                inv.occ[static_cast<size_t>(inv.vol.index(ox + ix, oy + iy, oz + iz))] = inst;
            }
        }
    }
    inv.held = -1;
    return true;
}

// Put the hand item back wherever it fits, same first-fit scan autoPlace uses.
// Returns false and keeps it in hand when nothing fits.
inline bool stowHeld(Inventory& inv, const ItemTable& table) {
    if (inv.held < 0) return false;
    const ItemDef* d = itemDefAt(table, inv.items[static_cast<size_t>(inv.held)].defIndex);
    if (!d) return false;
    for (int rot = 0; rot < 4; ++rot) {
        const ItemShape s = rotatedShape(d->shape, rot);
        for (int oz = 0; oz + s.sz <= inv.vol.sz; ++oz) {
            for (int oy = 0; oy + s.sy <= inv.vol.sy; ++oy) {
                for (int ox = 0; ox + s.sx <= inv.vol.sx; ++ox) {
                    if (!packFits(inv, s, ox, oy, oz)) continue;
                    // placeHeld re-checks handFits (which needs held to still be
                    // set) and clears held itself once the cells are written.
                    if (placeHeld(inv, table, rot, ox, oy, oz)) return true;
                }
            }
        }
    }
    return false;
}

// Spawn a fresh instance straight into the hand (no lattice scan). Used when a
// world pickup is refused because the pack is full: the item stays "carried"
// instead of vanishing.
inline bool takeIntoHand(Inventory& inv, const ItemTable& table, int defIndex) {
    if (inv.held >= 0) return false;
    const ItemDef* d = itemDefAt(table, defIndex);
    if (!d || !d->shape.valid()) return false;
    if (inv.items.size() >= 255) return false;
    ItemInstance inst;
    inst.defIndex = defIndex;
    inst.rounds = d->cls == ItemClass::AmmoPouch
                      ? d->rounds
                      : (d->cls == ItemClass::WeaponPrimary || d->cls == ItemClass::WeaponSmall
                             ? d->magazineSize
                             : 0);
    inv.items.push_back(inst);
    inv.held = static_cast<int>(inv.items.size()) - 1;
    return true;
}

// ---- equipment -----------------------------------------------------------

// Does `shape` fit entirely inside one box of `zone` under some Y rotation?
inline bool armorFitsZone(const ItemShape& shape, ArmorZone zone, int& outRot) {
    const auto& zd = armorZoneDef(zone);
    for (int rot = 0; rot < 4; ++rot) {
        ItemShape s = rotatedShape(shape, rot);
        for (const auto& b : zd.boxes) {
            for (int oz = b.z0; oz + s.sz - 1 <= b.z1; ++oz) {
                for (int oy = b.y0; oy + s.sy - 1 <= b.y1; ++oy) {
                    for (int ox = b.x0; ox + s.sx - 1 <= b.x1; ++ox) {
                        bool ok = true;
                        for (int iz = 0; iz < s.sz && ok; ++iz) {
                            for (int iy = 0; iy < s.sy && ok; ++iy) {
                                for (int ix = 0; ix < s.sx && ok; ++ix) {
                                    if (s.solid(ix, iy, iz) &&
                                        !b.contains(ox + ix, oy + iy, oz + iz))
                                        ok = false;
                                }
                            }
                        }
                        if (ok) {
                            outRot = rot;
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

// Which armor zones accept this piece? Returns false when it fits none.
inline bool armorAcceptingZones(const ItemDef& d, ArmorZone* outZones, int maxOut) {
    int n = 0;
    for (int i = 0; i < static_cast<int>(ArmorZone::Count); ++i) {
        const ArmorZone z = static_cast<ArmorZone>(i);
        if (d.armorZone >= 0 && d.armorZone != static_cast<int>(z)) continue;
        int rot = 0;
        if (armorFitsZone(d.shape, z, rot)) {
            if (outZones && n < maxOut) outZones[n] = z;
            ++n;
        }
    }
    return n > 0;
}

// Can this item be equipped in this slot? Resolves the armor zone for armor
// pieces and the class for everything else.
inline bool canEquip(const ItemTable& table, int defIndex, EquipSlot slot) {
    const ItemDef* d = itemDefAt(table, defIndex);
    if (!d) return false;
    if (!slotAcceptsClass(slot, d->cls)) return false;
    if (slotIsArmor(slot)) {
        const ArmorZone want = equipSlotZone(slot);
        // A piece bound to a specific zone may only go in that zone.
        if (d->armorZone >= 0 && d->armorZone != static_cast<int>(want)) return false;
        int rot = 0;
        return armorFitsZone(d->shape, want, rot);
    }
    return true;
}

// Equip into a slot, replacing whatever was there. The displaced item is
// returned via outDisplaced (def index, or -1).
inline bool equipDef(Inventory& inv, const ItemTable& table, int defIndex, EquipSlot slot,
                     int& outDisplaced) {
    outDisplaced = -1;
    if (!canEquip(table, defIndex, slot)) return false;
    const int si = static_cast<int>(slot);
    if (si < 0 || si >= kEquipSlotCount) return false;

    outDisplaced = inv.slotDef[si];
    inv.slotDef[si] = defIndex;
    int rot = 0;
    if (slotIsArmor(slot)) armorFitsZone(itemDefAt(table, defIndex)->shape, equipSlotZone(slot), rot);
    inv.slotRot[si] = rot;

    // A new backpack re-grants the storage volume; contents are dropped.
    if (slot == EquipSlot::Backpack) {
        const ItemDef* pack = itemDefAt(table, defIndex);
        PackVolume v;
        if (pack && pack->packSX > 0 && pack->packSY > 0 && pack->packSZ > 0) {
            v.sx = pack->packSX;
            v.sy = pack->packSY;
            v.sz = pack->packSZ;
        } else {
            // A Backpack-class item with no pack_size is malformed data. Grant
            // nothing rather than silently falling back to the base volume.
            v = PackVolume::none();
        }
        inv.setVolume(v);
    }
    return true;
}

inline bool unequipDef(Inventory& inv, EquipSlot slot, int& outDisplaced) {
    outDisplaced = -1;
    const int si = static_cast<int>(slot);
    if (si < 0 || si >= kEquipSlotCount) return false;
    outDisplaced = inv.slotDef[si];
    inv.slotDef[si] = -1;
    inv.slotRot[si] = 0;
    // Storage goes away with the pack. PackVolume{} would RESTORE the 3x3x4 base
    // volume, which is the opposite of unequipping.
    if (slot == EquipSlot::Backpack) inv.setVolume(PackVolume::none());
    return outDisplaced >= 0;
}

// Default loadout: base backpack into the Backpack slot.
inline void initInventory(Inventory& inv, const ItemTable& table) {
    inv.resetSlots();
    inv.setVolume(PackVolume{});
    const ItemDef* base = findItemById(table, "backpack_base");
    if (base) {
        int idx = -1;
        for (size_t i = 0; i < table.size(); ++i) {
            if (table[i].id == base->id) {
                idx = static_cast<int>(i);
                break;
            }
        }
        int displaced = -1;
        if (idx >= 0) equipDef(inv, table, idx, EquipSlot::Backpack, displaced);
    }
}

// ---- item JSON -----------------------------------------------------------

// Collect every integer in the array body after "key". Handles both flat
// arrays ([3,3,4]) and nested ones ([[0,0,0],[1,0,0]]) by flattening, which is
// all our controlled exporter emits.
inline std::vector<int> jsonExtractIntArray(const std::string& text, const char* key) {
    const std::string body = jsonExtractArrayBody(text, key);
    std::vector<int> out;
    size_t i = 0;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '-' || (c >= '0' && c <= '9')) {
            size_t j = i;
            if (body[j] == '-') ++j;
            while (j < body.size() && body[j] >= '0' && body[j] <= '9') ++j;
            try {
                out.push_back(std::stoi(body.substr(i, j - i)));
            } catch (...) {
            }
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

inline std::vector<float> jsonExtractFloatArray(const std::string& text, const char* key) {
    const std::string body = jsonExtractArrayBody(text, key);
    std::vector<float> out;
    size_t i = 0;
    while (i < body.size()) {
        const char c = body[i];
        if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
            size_t j = i;
            if (body[j] == '-' || body[j] == '+') ++j;
            while (j < body.size() && (body[j] == '.' || body[j] == 'e' || body[j] == 'E' ||
                                       (body[j] >= '0' && body[j] <= '9') || body[j] == '-' ||
                                       body[j] == '+'))
                ++j;
            try {
                out.push_back(std::stof(body.substr(i, j - i)));
            } catch (...) {
            }
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

inline ItemDef parseItemObject(const std::string& text) {
    ItemDef d;
    std::string obj = text;
    const size_t start = text.find('{');
    const size_t end = text.rfind('}');
    if (start != std::string::npos && end != std::string::npos && end > start)
        obj = text.substr(start, end - start + 1);

    d.id = jsonExtractString(obj, "id", "");
    d.name = jsonExtractString(obj, "name", d.id);
    d.cls = itemClassFromName(jsonExtractString(obj, "class", "misc"));
    d.material = materialFromName(jsonExtractString(obj, "material", "sheet_metal"));
    d.unit = static_cast<int>(jsonExtractFloat(obj, "unit", 1.0f));
    d.voxelSize = jsonExtractFloat(obj, "voxel_size", 0.001f);

    const std::vector<float> col = jsonExtractFloatArray(obj, "color");
    if (col.size() >= 3) {
        d.cr = col[0];
        d.cg = col[1];
        d.cb = col[2];
    }

    const std::vector<int> size = jsonExtractIntArray(obj, "size");
    int sx = 0, sy = 0, sz = 0;
    if (size.size() >= 3) {
        sx = size[0];
        sy = size[1];
        sz = size[2];
    }

    const std::vector<int> cells = jsonExtractIntArray(obj, "cells");
    if (!cells.empty() && cells.size() >= 3 && cells.size() % 3 == 0) {
        d.shape = makeShapeFromCells(sx, sy, sz, cells);
    } else {
        d.shape = makeBoxShape(sx, sy, sz);
    }

    d.weaponId = jsonExtractString(obj, "weapon_id", "");
    d.ammoId = jsonExtractString(obj, "ammo_id", "");
    d.magazineSize = static_cast<int>(jsonExtractFloat(obj, "magazine_size", 0.0f));
    // Only canonicalize a caliber that was actually authored; normalizeCaliber
    // maps "" to "medium", which would silently mislabel caliber-less items.
    const std::string rawCal = jsonExtractString(obj, "caliber", "");
    d.caliber = rawCal.empty() ? std::string() : normalizeCaliber(rawCal);
    d.rounds = static_cast<int>(jsonExtractFloat(obj, "rounds", 0.0f));

    const std::vector<int> pack = jsonExtractIntArray(obj, "pack_size");
    if (pack.size() >= 3) {
        d.packSX = pack[0];
        d.packSY = pack[1];
        d.packSZ = pack[2];
    }
    d.armorZone = armorZoneFromName(jsonExtractString(obj, "armor_zone", ""));
    return d;
}

inline ItemDef loadItemDef(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        ItemDef miss;
        miss.id.clear();
        return miss;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    ItemDef d = parseItemObject(ss.str());
    if (!itemGridValid(d)) {
        ItemDef bad;
        bad.id.clear();
        return bad;
    }
    return d;
}
