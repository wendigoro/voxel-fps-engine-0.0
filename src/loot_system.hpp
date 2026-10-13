#pragma once
// Cascading Loot Drop & Container Filtering System (Simulation Authority)
//
// Implements a 4-tier quality scale (Junk, Worn, Pristine, Elite) across 4 categories
// (Gear/Armor, Contraband/Valuables, Weapons, Weapon Parts).
// Evaluates quality rolls from highest tier to lowest tier (Elite -> Pristine -> Worn -> Junk)
// and applies container-specific filter tables to bias item categories.
//
// Governed by RULES.md: Authoritative on the simulation side, deterministic PRNG.

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>

namespace loot {

// 4 Quality / Condition Tiers
enum class LootTier : uint8_t {
    Junk     = 0, // Worn-out, damaged, scrap, lowest value
    Worn     = 1, // Field standard, moderate wear
    Pristine = 2, // Factory new, high grade
    Elite    = 3  // Prototype, mastercrafted, apex grade
};

inline const char* tierToString(LootTier t) {
    switch (t) {
        case LootTier::Junk:     return "Junk";
        case LootTier::Worn:     return "Worn";
        case LootTier::Pristine: return "Pristine";
        case LootTier::Elite:    return "Elite";
        default:                 return "Unknown";
    }
}

// 4 Item Categories
enum class ItemCategory : uint8_t {
    GearArmor  = 0, // Helmets, chest plates, leg plates, backpacks, ammo pouches
    Contraband = 1, // Valuables, intel, currency chips, precious metals, encrypted drives
    Weapon     = 2, // Primary weapons, sidearms, heavy ordnance
    WeaponPart = 3  // Receivers, barrels, magazines, optics, stocks, triggers
};

inline const char* categoryToString(ItemCategory c) {
    switch (c) {
        case ItemCategory::GearArmor:  return "GearArmor";
        case ItemCategory::Contraband: return "Contraband";
        case ItemCategory::Weapon:     return "Weapon";
        case ItemCategory::WeaponPart: return "WeaponPart";
        default:                       return "Unknown";
    }
}

// Container Types
enum class ContainerType : uint8_t {
    WeaponLocker       = 0, // High weapon & part weight, 0 contraband
    WallSafe           = 1, // High contraband & valuable weight, 0 armor
    MilitaryFootlocker = 2, // Armor & military weapons, tactical parts
    ScrapBin           = 3, // Parts, scrap metal, junk/worn commodities
    CovertDeadDrop     = 4, // High-tier contraband & covert sidearms
    MedicalCabinet     = 5  // Field gear, medical supplies, stims
};

inline const char* containerTypeToString(ContainerType t) {
    switch (t) {
        case ContainerType::WeaponLocker:       return "WeaponLocker";
        case ContainerType::WallSafe:           return "WallSafe";
        case ContainerType::MilitaryFootlocker: return "MilitaryFootlocker";
        case ContainerType::ScrapBin:           return "ScrapBin";
        case ContainerType::CovertDeadDrop:     return "CovertDeadDrop";
        case ContainerType::MedicalCabinet:     return "MedicalCabinet";
        default:                                return "Unknown";
    }
}

// Individual Catalog Item Entry
struct LootItemEntry {
    std::string id;
    std::string name;
    ItemCategory category;
    LootTier tier;
    float valueCredits;
    int volumeCells; // Unit-cube volume in inventory grid
};

// Master Loot Item Catalog
inline const std::vector<LootItemEntry>& getLootCatalog() {
    static const std::vector<LootItemEntry> kCatalog = {
        // ---- Gear & Armor ----------------------------------------------------
        // Elite
        { "armor_reactive_chest",       "Reactive Composite Chest",     ItemCategory::GearArmor,  LootTier::Elite,    8500.0f,  42 },
        { "armor_exoskeleton_legs",     "Pneumatic Exoskeleton Greaves",ItemCategory::GearArmor,  LootTier::Elite,    7200.0f,  36 },
        { "backpack_military_spec",     "Mil-Spec Heavy Rucksack",      ItemCategory::GearArmor,  LootTier::Elite,    5500.0f,  80 },
        // Pristine
        { "armor_chest_plate_pristine", "Polymer Plate Carrier",        ItemCategory::GearArmor,  LootTier::Pristine, 3200.0f,  35 },
        { "armor_helmet_tactical",      "Kevlar Ballistic Helmet",      ItemCategory::GearArmor,  LootTier::Pristine, 2400.0f,  18 },
        { "backpack_field_upgrade",     "Field Tactical Backpack",      ItemCategory::GearArmor,  LootTier::Pristine, 2800.0f,  61 },
        // Worn
        { "armor_chest_plate",          "Standard Issue Chest Plate",   ItemCategory::GearArmor,  LootTier::Worn,     1200.0f,  30 },
        { "armor_helmet",               "Standard Steel Helmet",        ItemCategory::GearArmor,  LootTier::Worn,      800.0f,  16 },
        { "armor_arm_guard",            "Reinforced Arm Guard",         ItemCategory::GearArmor,  LootTier::Worn,      650.0f,  12 },
        { "backpack_base",              "Standard Canvas Backpack",     ItemCategory::GearArmor,  LootTier::Worn,      950.0f,  36 },
        { "ammo_pouch_medium",          "MOLLE Medium Ammo Pouch",      ItemCategory::GearArmor,  LootTier::Worn,      450.0f,  10 },
        // Junk
        { "armor_scrap_vest",           "Improvised Scrap Vest",        ItemCategory::GearArmor,  LootTier::Junk,      250.0f,  24 },
        { "armor_makeshift_plate",      "Welded Rebar Plate",           ItemCategory::GearArmor,  LootTier::Junk,      180.0f,  15 },
        { "tattered_satchel",           "Tattered Duffel Bag",          ItemCategory::GearArmor,  LootTier::Junk,      120.0f,  16 },

        // ---- Contraband & Valuables ------------------------------------------
        // Elite
        { "contraband_prototype_core",  "Quantum Prototype Core",       ItemCategory::Contraband, LootTier::Elite,   25000.0f,   8 },
        { "contraband_gold_bullion",    "999 Fine Gold Bullion Ingot",  ItemCategory::Contraband, LootTier::Elite,   18000.0f,  12 },
        { "contraband_crypto_ledger",   "Hardware Crypto Cold Storage", ItemCategory::Contraband, LootTier::Elite,   15000.0f,   4 },
        // Pristine
        { "contraband_bearer_bond",     "Corporate Bearer Bond Bond",   ItemCategory::Contraband, LootTier::Pristine, 7200.0f,   2 },
        { "contraband_classified_intel","Encrypted Optical Intel Drive",ItemCategory::Contraband, LootTier::Pristine, 6500.0f,   4 },
        { "contraband_rare_mineral",    "Refined Titanium Pellets",     ItemCategory::Contraband, LootTier::Pristine, 5800.0f,   6 },
        // Worn
        { "contraband_silver_bar",      "Refined Silver Bar (1kg)",     ItemCategory::Contraband, LootTier::Worn,     2800.0f,   8 },
        { "contraband_encrypted_drive", "Corrupted Magnetic Data Tape", ItemCategory::Contraband, LootTier::Worn,     2200.0f,   4 },
        { "contraband_counterfeit_pass","Forged Security Keycard",      ItemCategory::Contraband, LootTier::Worn,     1500.0f,   2 },
        // Junk
        { "contraband_scrap_copper",    "Salvaged Copper Wire Spool",   ItemCategory::Contraband, LootTier::Junk,      450.0f,   6 },
        { "contraband_debased_coin",    "Bag of Debased Coins",         ItemCategory::Contraband, LootTier::Junk,      300.0f,   4 },
        { "contraband_ration_stamp",    "Expired Emergency Ration Token",ItemCategory::Contraband,LootTier::Junk,      150.0f,   1 },

        // ---- Weapons ---------------------------------------------------------
        // Elite
        { "weapon_railgun_energy",      "Hypervelocity Railgun",        ItemCategory::Weapon,     LootTier::Elite,   32000.0f,  48 },
        { "weapon_cannon_heavy",        "Autocannon Heavy Ordnance",    ItemCategory::Weapon,     LootTier::Elite,   28000.0f,  54 },
        { "weapon_sniper_heavy",        "Anti-Material Heavy Sniper",   ItemCategory::Weapon,     LootTier::Elite,   22000.0f,  45 },
        // Pristine
        { "weapon_marksman_rifle",      "Designated Marksman Rifle",    ItemCategory::Weapon,     LootTier::Pristine,12000.0f,  38 },
        { "weapon_carbine_energy",      "Plasma Burst Carbine",         ItemCategory::Weapon,     LootTier::Pristine,11500.0f,  32 },
        { "weapon_lmg_medium",          "Squad Automatic LMG",          ItemCategory::Weapon,     LootTier::Pristine,13000.0f,  44 },
        // Worn
        { "weapon_starter_rifle",       "Service Assault Rifle",        ItemCategory::Weapon,     LootTier::Worn,     4500.0f,  32 },
        { "weapon_shotgun_light",       "Pump-Action Trench Shotgun",   ItemCategory::Weapon,     LootTier::Worn,     3800.0f,  28 },
        { "weapon_smg_light",           "Compact Tactical SMG",         ItemCategory::Weapon,     LootTier::Worn,     3500.0f,  22 },
        // Junk
        { "weapon_pipe_scattergun",     "Zip-Gun Pipe Scattergun",      ItemCategory::Weapon,     LootTier::Junk,     1100.0f,  20 },
        { "weapon_sidearm_light",       "Pitted Surplus Sidearm",       ItemCategory::Weapon,     LootTier::Junk,      900.0f,  12 },

        // ---- Weapon Parts ----------------------------------------------------
        // Elite
        { "part_match_grade_barrel",    "Cold-Hammered Match Barrel",   ItemCategory::WeaponPart, LootTier::Elite,    6800.0f,  14 },
        { "part_thermal_optic_4x",      "4x Thermal Imaging Scope",     ItemCategory::WeaponPart, LootTier::Elite,    8500.0f,  10 },
        { "part_hairpin_trigger",       "Hairpin Match Trigger Group",  ItemCategory::WeaponPart, LootTier::Elite,    4800.0f,   4 },
        // Pristine
        { "part_extended_drum_mag",     "High-Capacity Drum Magazine",  ItemCategory::WeaponPart, LootTier::Pristine, 3200.0f,   8 },
        { "part_holo_sight",            "Holographic Reflex Sight",     ItemCategory::WeaponPart, LootTier::Pristine, 2600.0f,   6 },
        { "part_reinforced_bolt",       "Titanium Nitride Bolt Carrier",ItemCategory::WeaponPart, LootTier::Pristine, 2900.0f,   6 },
        // Worn
        { "part_standard_magazine",     "Steel Box Magazine (30-rnd)",  ItemCategory::WeaponPart, LootTier::Worn,     1100.0f,   6 },
        { "part_polymer_stock",         "Collapsible Polymer Stock",    ItemCategory::WeaponPart, LootTier::Worn,      950.0f,  10 },
        { "part_flash_hider",           "Slotted Flash Compensator",    ItemCategory::WeaponPart, LootTier::Worn,      700.0f,   3 },
        // Junk
        { "part_rusty_receiver",        "Corroded Upper Receiver",      ItemCategory::WeaponPart, LootTier::Junk,      350.0f,  12 },
        { "part_cracked_grip",          "Cracked Bakelite Grip",        ItemCategory::WeaponPart, LootTier::Junk,      180.0f,   4 },
        { "part_worn_spring",           "Fatigued Recoil Spring",       ItemCategory::WeaponPart, LootTier::Junk,      120.0f,   2 }
    };
    return kCatalog;
}

// Container Profile Definition
struct ContainerProfile {
    ContainerType type;
    const char* name;

    // Category distribution weights (relative probabilities, sum can be arbitrary > 0)
    float weightGearArmor;
    float weightContraband;
    float weightWeapon;
    float weightWeaponPart;

    // Cascading roll probabilities (tested strictly top-down: Elite -> Pristine -> Worn -> Junk fallback)
    float pElite;
    float pPristine;
    float pWorn;

    // Min and max drop count per opening
    int minDrops;
    int maxDrops;
};

// Table of Container Profiles
inline const ContainerProfile& getContainerProfile(ContainerType type) {
    static const ContainerProfile kProfiles[] = {
        // WeaponLocker: 60% Weapons, 35% Parts, 5% Armor, 0% Contraband
        { ContainerType::WeaponLocker, "Weapon Locker",
          0.05f, 0.00f, 0.60f, 0.35f,
          0.08f, 0.28f, 0.50f, 1, 3 },

        // WallSafe: 80% Contraband, 15% Parts, 5% Weapons, 0% Armor
        { ContainerType::WallSafe, "Wall Safe",
          0.00f, 0.80f, 0.05f, 0.15f,
          0.15f, 0.40f, 0.35f, 2, 4 },

        // MilitaryFootlocker: 45% Armor, 35% Weapons, 20% Parts, 0% Contraband
        { ContainerType::MilitaryFootlocker, "Military Footlocker",
          0.45f, 0.00f, 0.35f, 0.20f,
          0.10f, 0.35f, 0.45f, 2, 4 },

        // ScrapBin: 50% Parts, 30% Contraband/Scrap, 20% Armor, 0% Weapons
        { ContainerType::ScrapBin, "Scrap Bin",
          0.20f, 0.30f, 0.00f, 0.50f,
          0.01f, 0.09f, 0.40f, 1, 3 },

        // CovertDeadDrop: 60% Contraband, 30% Weapons, 10% Parts, 0% Armor
        { ContainerType::CovertDeadDrop, "Covert Dead Drop",
          0.00f, 0.60f, 0.30f, 0.10f,
          0.25f, 0.45f, 0.25f, 1, 2 },

        // MedicalCabinet: 70% Gear/Armor (vests/satchels), 30% Contraband, 0% Weapons, 0% Parts
        { ContainerType::MedicalCabinet, "Medical Cabinet",
          0.70f, 0.30f, 0.00f, 0.00f,
          0.05f, 0.30f, 0.50f, 2, 4 }
    };
    const size_t idx = static_cast<size_t>(type);
    if (idx < sizeof(kProfiles) / sizeof(kProfiles[0])) {
        return kProfiles[idx];
    }
    return kProfiles[0];
}

// Fast Deterministic Pseudo-Random Number Generator (Simulation Authority)
class LootRng {
public:
    uint64_t state;

    explicit LootRng(uint64_t seed = 0x853c49e6748fea9bULL) : state(seed ? seed : 0x853c49e6748fea9bULL) {}

    uint32_t next() {
        // 64-bit LCG step
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(state >> 32);
    }

    // Returns float in [0.0, 1.0)
    float nextFloat() {
        return static_cast<float>(next()) / 4294967296.0f;
    }

    // Returns integer in [minVal, maxVal]
    int nextInt(int minVal, int maxVal) {
        if (minVal >= maxVal) return minVal;
        const uint32_t range = static_cast<uint32_t>(maxVal - minVal + 1);
        return minVal + static_cast<int>(next() % range);
    }
};

// Cascading Tier Roller: Evaluates quality from highest to lowest
inline LootTier rollCascadingTier(const ContainerProfile& profile, LootRng& rng) {
    // 1. Roll Elite (Apex tier)
    if (rng.nextFloat() < profile.pElite) {
        return LootTier::Elite;
    }
    // 2. Roll Pristine (High tier)
    if (rng.nextFloat() < profile.pPristine) {
        return LootTier::Pristine;
    }
    // 3. Roll Worn (Standard field tier)
    if (rng.nextFloat() < profile.pWorn) {
        return LootTier::Worn;
    }
    // 4. Fallback to Junk
    return LootTier::Junk;
}

// Container Category Filter: Rolls an item category using the container's filter weights
inline ItemCategory rollFilteredCategory(const ContainerProfile& profile, LootRng& rng) {
    const float totalWeight = profile.weightGearArmor +
                              profile.weightContraband +
                              profile.weightWeapon +
                              profile.weightWeaponPart;

    if (totalWeight <= 0.0f) {
        return ItemCategory::GearArmor; // Safety default
    }

    const float pick = rng.nextFloat() * totalWeight;
    float accum = 0.0f;

    accum += profile.weightGearArmor;
    if (pick < accum && profile.weightGearArmor > 0.0f) return ItemCategory::GearArmor;

    accum += profile.weightContraband;
    if (pick < accum && profile.weightContraband > 0.0f) return ItemCategory::Contraband;

    accum += profile.weightWeapon;
    if (pick < accum && profile.weightWeapon > 0.0f) return ItemCategory::Weapon;

    return ItemCategory::WeaponPart;
}

// Finds all catalog items matching the selected (category, tier)
inline std::vector<size_t> findMatchingItems(ItemCategory cat, LootTier tier) {
    const auto& catalog = getLootCatalog();
    std::vector<size_t> indices;
    for (size_t i = 0; i < catalog.size(); ++i) {
        if (catalog[i].category == cat && catalog[i].tier == tier) {
            indices.push_back(i);
        }
    }
    return indices;
}

// Generates a single item drop from a container profile
inline LootItemEntry generateSingleDrop(const ContainerProfile& profile, LootRng& rng) {
    const ItemCategory cat = rollFilteredCategory(profile, rng);
    const LootTier tier = rollCascadingTier(profile, rng);

    std::vector<size_t> candidates = findMatchingItems(cat, tier);

    // If tier has no items in category, fallback down the tiers:
    if (candidates.empty() && tier != LootTier::Junk) {
        // Fallback to Worn
        candidates = findMatchingItems(cat, LootTier::Worn);
    }
    if (candidates.empty()) {
        // Fallback to Junk
        candidates = findMatchingItems(cat, LootTier::Junk);
    }
    if (candidates.empty()) {
        // Fallback across any tier in category
        for (size_t i = 0; i < getLootCatalog().size(); ++i) {
            if (getLootCatalog()[i].category == cat) candidates.push_back(i);
        }
    }

    if (!candidates.empty()) {
        const size_t pickIdx = candidates[static_cast<size_t>(rng.nextInt(0, static_cast<int>(candidates.size() - 1)))];
        return getLootCatalog()[pickIdx];
    }

    // Absolute fallback
    return getLootCatalog()[0];
}

// Opens a container, generating a collection of dropped items based on container type & seed
inline std::vector<LootItemEntry> openContainer(ContainerType type, uint64_t seed) {
    const ContainerProfile& profile = getContainerProfile(type);
    LootRng rng(seed);

    const int dropCount = rng.nextInt(profile.minDrops, profile.maxDrops);
    std::vector<LootItemEntry> drops;
    drops.reserve(static_cast<size_t>(dropCount));

    for (int i = 0; i < dropCount; ++i) {
        drops.push_back(generateSingleDrop(profile, rng));
    }
    return drops;
}

// =========================================================================
// Autonomous Self-Test Suite (Wired to Exit Code 16)
// =========================================================================
inline bool selfTest() {
    std::cout << "== loot::selfTest() ==" << std::endl;

    // Test 1: Catalog Completeness across all 16 (Category, Tier) Combinations
    {
        for (int c = 0; c < 4; ++c) {
            for (int t = 0; t < 4; ++t) {
                const ItemCategory cat = static_cast<ItemCategory>(c);
                const LootTier tier = static_cast<LootTier>(t);
                const std::vector<size_t> matches = findMatchingItems(cat, tier);
                if (matches.empty()) {
                    std::cerr << "FAIL: Loot catalog missing items for ("
                              << categoryToString(cat) << ", " << tierToString(tier) << ")" << std::endl;
                    return false;
                }
            }
        }
    }

    // Test 2: Container Category Filtering Contract
    // Asserts that container profiles strictly disallow 0-weight categories.
    {
        constexpr int kSamples = 10000;

        // Weapon Locker: 0% Contraband
        {
            LootRng rng(101);
            const ContainerProfile& prof = getContainerProfile(ContainerType::WeaponLocker);
            for (int i = 0; i < kSamples; ++i) {
                const LootItemEntry drop = generateSingleDrop(prof, rng);
                if (drop.category == ItemCategory::Contraband) {
                    std::cerr << "FAIL: WeaponLocker generated contraband: " << drop.id << std::endl;
                    return false;
                }
            }
        }

        // Wall Safe: 0% Gear/Armor
        {
            LootRng rng(202);
            const ContainerProfile& prof = getContainerProfile(ContainerType::WallSafe);
            for (int i = 0; i < kSamples; ++i) {
                const LootItemEntry drop = generateSingleDrop(prof, rng);
                if (drop.category == ItemCategory::GearArmor) {
                    std::cerr << "FAIL: WallSafe generated GearArmor: " << drop.id << std::endl;
                    return false;
                }
            }
        }

        // Scrap Bin: 0% Weapons
        {
            LootRng rng(303);
            const ContainerProfile& prof = getContainerProfile(ContainerType::ScrapBin);
            for (int i = 0; i < kSamples; ++i) {
                const LootItemEntry drop = generateSingleDrop(prof, rng);
                if (drop.category == ItemCategory::Weapon) {
                    std::cerr << "FAIL: ScrapBin generated weapon: " << drop.id << std::endl;
                    return false;
                }
            }
        }
    }

    // Test 3: Cascading Roll Order & Distribution Invariance
    // Verifies that Elite, Pristine, Worn, and Junk rolls produce positive frequencies
    // in descending expected tiers according to profile probabilities.
    {
        constexpr int kRolls = 20000;
        LootRng rng(404);
        const ContainerProfile& prof = getContainerProfile(ContainerType::MilitaryFootlocker);
        // MilitaryFootlocker: pElite=0.10, pPristine=0.35, pWorn=0.45, remainder Junk

        int counts[4] = { 0, 0, 0, 0 }; // Junk, Worn, Pristine, Elite
        for (int i = 0; i < kRolls; ++i) {
            const LootTier t = rollCascadingTier(prof, rng);
            counts[static_cast<size_t>(t)]++;
        }

        const float fElite = static_cast<float>(counts[3]) / kRolls;
        const float fPristine = static_cast<float>(counts[2]) / kRolls;
        const float fWorn = static_cast<float>(counts[1]) / kRolls;
        const float fJunk = static_cast<float>(counts[0]) / kRolls;

        // Elite target ~ 10%
        if (fElite < 0.07f || fElite > 0.13f) {
            std::cerr << "FAIL: Elite roll frequency out of bounds: " << fElite << std::endl;
            return false;
        }
        // Pristine target ~ (1 - 0.10) * 0.35 = 31.5%
        if (fPristine < 0.27f || fPristine > 0.36f) {
            std::cerr << "FAIL: Pristine roll frequency out of bounds: " << fPristine << std::endl;
            return false;
        }
        // Worn target ~ (1 - 0.10 - 0.315) * 0.45 = 26.3%
        if (fWorn < 0.22f || fWorn > 0.31f) {
            std::cerr << "FAIL: Worn roll frequency out of bounds: " << fWorn << std::endl;
            return false;
        }
        // Junk fallback > 0
        if (fJunk < 0.25f || fJunk > 0.38f) {
            std::cerr << "FAIL: Junk roll frequency out of bounds: " << fJunk << std::endl;
            return false;
        }
    }

    // Test 4: Deterministic Seeding Reproducibility
    {
        const std::vector<LootItemEntry> runA = openContainer(ContainerType::CovertDeadDrop, 0x12345678ULL);
        const std::vector<LootItemEntry> runB = openContainer(ContainerType::CovertDeadDrop, 0x12345678ULL);
        const std::vector<LootItemEntry> runC = openContainer(ContainerType::CovertDeadDrop, 0x87654321ULL);

        if (runA.size() != runB.size()) {
            std::cerr << "FAIL: Deterministic container size mismatch" << std::endl;
            return false;
        }
        for (size_t i = 0; i < runA.size(); ++i) {
            if (runA[i].id != runB[i].id || runA[i].tier != runB[i].tier) {
                std::cerr << "FAIL: Deterministic item identity mismatch at index " << i << std::endl;
                return false;
            }
        }
        // Different seed should vary
        bool diff = (runA.size() != runC.size());
        if (!diff) {
            for (size_t i = 0; i < runA.size(); ++i) {
                if (runA[i].id != runC[i].id) { diff = true; break; }
            }
        }
        if (!diff) {
            std::cerr << "FAIL: Different seeds produced identical drops" << std::endl;
            return false;
        }
    }

    // Test 5: Drop Count Bounds Enforced
    {
        for (int c = 0; c < 6; ++c) {
            const ContainerType ct = static_cast<ContainerType>(c);
            const ContainerProfile& prof = getContainerProfile(ct);
            for (uint64_t s = 1; s <= 50; ++s) {
                const std::vector<LootItemEntry> drops = openContainer(ct, s * 9973);
                if (static_cast<int>(drops.size()) < prof.minDrops ||
                    static_cast<int>(drops.size()) > prof.maxDrops) {
                    std::cerr << "FAIL: Container " << prof.name << " drop count out of bounds: "
                              << drops.size() << " (bounds: [" << prof.minDrops << "," << prof.maxDrops << "])" << std::endl;
                    return false;
                }
            }
        }
    }

    std::cout << "loot::selfTest(): PASS (All 5 test vectors OK)" << std::endl;
    return true;
}

} // namespace loot
