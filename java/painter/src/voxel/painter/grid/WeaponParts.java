package voxel.painter.grid;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/** Weapon part classification + stat composition + JSON export. */
public final class WeaponParts {
    public static final int NONE = 0;
    public static final int BARREL = 1;       // damage
    public static final int ACTION = 2;       // impact
    public static final int BOLT_CHAMBER = 3; // recoil
    public static final int TRIGGER = 4;      // handling
    public static final int GRIP_STOCK = 5;   // weight
    public static final int SIGHT = 6;        // optic

    public static String name(int id) {
        return switch (id) {
            case BARREL -> "barrel";
            case ACTION -> "action";
            case BOLT_CHAMBER -> "bolt_chamber";
            case TRIGGER -> "trigger";
            case GRIP_STOCK -> "grip_stock";
            case SIGHT -> "sight";
            default -> "none";
        };
    }

    public static int idFromName(String s) {
        if (s == null) return NONE;
        return switch (s.toLowerCase(Locale.ROOT)) {
            case "barrel" -> BARREL;
            case "action" -> ACTION;
            case "bolt_chamber", "bolt", "chamber" -> BOLT_CHAMBER;
            case "trigger" -> TRIGGER;
            case "grip_stock", "grip", "stock" -> GRIP_STOCK;
            case "sight", "optic" -> SIGHT;
            default -> NONE;
        };
    }

    public static final class Stats {
        public float damage, impact, recoil, handling, weight, optic;
        public String caliber = "medium";
        public String fireMode = "semi"; // semi | auto | bolt
        public boolean hitscan;
        public Map<String, Integer> partCounts = new LinkedHashMap<>();
    }

    public static Stats compose(VoxelGrid g, String caliber) {
        g.assertCubicUnitInvariant();
        Stats s = new Stats();
        s.caliber = caliber == null ? "medium" : caliber;
        s.hitscan = "energy".equalsIgnoreCase(s.caliber)
                || "energy_beam".equalsIgnoreCase(s.caliber);
        // Default cadence from part balance: long barrel+bolt leans bolt-action; light trigger leans auto.
        s.fireMode = "semi";
        int[] counts = new int[7];
        float[] mass = new float[7];
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++) {
                    int m = g.getMat(x, y, z);
                    if (m == MaterialPalette.AIR) continue;
                    int p = g.getPart(x, y, z);
                    if (p < 1 || p > 6) p = GRIP_STOCK; // unlabeled solid counts as furniture mass
                    counts[p]++;
                    mass[p] += MaterialPalette.density(m) * MaterialPalette.weightMul(m);
                }
        for (int p = 1; p <= 6; p++) s.partCounts.put(name(p), counts[p]);
        // Role weights — simplified grain/caliber-ready scales
        s.damage = 4f + counts[BARREL] * 1.8f + mass[BARREL] * 0.35f;
        s.impact = 3f + counts[ACTION] * 1.5f + mass[ACTION] * 0.4f;
        s.recoil = 2f + counts[BOLT_CHAMBER] * 1.2f + mass[BOLT_CHAMBER] * 0.5f;
        s.handling = 5f + counts[TRIGGER] * 1.1f - mass[GRIP_STOCK] * 0.15f + counts[GRIP_STOCK] * 0.2f;
        s.weight = 1f + mass[GRIP_STOCK] * 0.55f + mass[BARREL] * 0.25f + mass[ACTION] * 0.2f;
        s.optic = counts[SIGHT] * 2.5f + (hasPlexiSight(g) ? 3f : 0f);
        if (s.hitscan) {
            s.recoil *= 0.35f;
            s.damage *= 1.1f;
        } else if ("light".equalsIgnoreCase(s.caliber)) {
            s.damage *= 0.85f; s.recoil *= 0.7f; s.weight *= 0.85f;
        } else if ("heavy".equalsIgnoreCase(s.caliber)) {
            s.damage *= 1.35f; s.recoil *= 1.4f; s.impact *= 1.2f; s.weight *= 1.25f;
        }
        s.handling = Math.max(0.5f, s.handling);
        if (counts[BOLT_CHAMBER] >= 3 && counts[BARREL] >= 10) s.fireMode = "bolt";
        else if (counts[TRIGGER] >= 2 && counts[BOLT_CHAMBER] <= 1) s.fireMode = "auto";
        else s.fireMode = "semi";
        return s;
    }

    private static boolean hasPlexiSight(VoxelGrid g) {
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++)
                    if (g.getPart(x, y, z) == SIGHT && g.getMat(x, y, z) == MaterialPalette.PLEXIGLASS)
                        return true;
        return false;
    }

    /** Minimal cubic unit rifle template for smoke. */
    public static void paintStarterRifle(VoxelGrid g) {
        g.clear();
        // stock / grip treated wood
        for (int x = 2; x <= 6; x++)
            for (int y = 2; y <= 4; y++)
                g.set(x, y, 4, MaterialPalette.TREATED_WOOD, MaterialPalette.defaultRgb(MaterialPalette.TREATED_WOOD), GRIP_STOCK);
        // action / receiver sheet metal
        for (int x = 7; x <= 10; x++)
            for (int y = 3; y <= 5; y++)
                g.set(x, y, 4, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        // bolt/chamber
        g.set(9, 5, 4, MaterialPalette.GIRDER, MaterialPalette.defaultRgb(MaterialPalette.GIRDER), BOLT_CHAMBER);
        g.set(10, 5, 4, MaterialPalette.GIRDER, MaterialPalette.defaultRgb(MaterialPalette.GIRDER), BOLT_CHAMBER);
        // barrel carbon
        for (int x = 11; x <= 18; x++)
            g.set(x, 4, 4, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
        // trigger
        g.set(8, 2, 4, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), TRIGGER);
        // sight plexiglass
        g.set(12, 6, 4, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
        g.set(13, 6, 4, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintSidearmLight(VoxelGrid g) {
        g.clear();
        int cz = g.sizeZ() / 2;
        for (int x = 2; x <= 5; x++)
            for (int y = 1; y <= 4; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.TREATED_WOOD, MaterialPalette.defaultRgb(MaterialPalette.TREATED_WOOD), GRIP_STOCK);
        g.set(6, 2, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), TRIGGER);
        g.set(6, 3, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), TRIGGER);
        for (int x = 4; x <= 14; x++)
            for (int y = 4; y <= 5; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 5; x <= 13; x++)
            for (int z = cz - 1; z <= cz + 1; z++)
                g.set(x, 6, z, (x >= 7 && x <= 8) ? MaterialPalette.GIRDER : MaterialPalette.SHEET_METAL,
                        MaterialPalette.defaultRgb((x >= 7 && x <= 8) ? MaterialPalette.GIRDER : MaterialPalette.SHEET_METAL),
                        (x >= 7 && x <= 8) ? BOLT_CHAMBER : ACTION);
        for (int x = 14; x <= 16; x++)
            g.set(x, 5, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
        g.set(5, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
        g.set(14, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintSmgLight(VoxelGrid g) {
        g.clear();
        int cz = g.sizeZ() / 2;
        for (int x = 1; x <= 5; x++) {
            g.set(x, 5, cz - 1, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), GRIP_STOCK);
            g.set(x, 5, cz + 1, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), GRIP_STOCK);
        }
        for (int x = 5; x <= 7; x++)
            for (int y = 1; y <= 4; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), GRIP_STOCK);
        g.set(8, 2, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), TRIGGER);
        for (int x = 9; x <= 10; x++)
            for (int y = 0; y <= 3; y++)
                g.set(x, y, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 6; x <= 16; x++)
            for (int y = 5; y <= 6; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 10; x <= 12; x++)
            g.set(x, 7, cz, MaterialPalette.GIRDER, MaterialPalette.defaultRgb(MaterialPalette.GIRDER), BOLT_CHAMBER);
        for (int x = 17; x <= 22; x++)
            for (int z = cz - 1; z <= cz + 1; z++)
                g.set(x, 5, z, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
        g.set(11, 8, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
        g.set(18, 8, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintShotgunLight(VoxelGrid g) {
        g.clear();
        int cz = g.sizeZ() / 2;
        for (int x = 1; x <= 7; x++)
            for (int y = 2; y <= 4; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.TREATED_WOOD, MaterialPalette.defaultRgb(MaterialPalette.TREATED_WOOD), GRIP_STOCK);
        for (int x = 7; x <= 8; x++)
            for (int y = 1; y <= 3; y++)
                g.set(x, y, cz, MaterialPalette.TREATED_WOOD, MaterialPalette.defaultRgb(MaterialPalette.TREATED_WOOD), GRIP_STOCK);
        g.set(9, 2, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), TRIGGER);
        for (int x = 9; x <= 15; x++)
            for (int y = 3; y <= 6; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 11; x <= 13; x++)
            g.set(x, 6, cz, MaterialPalette.GIRDER, MaterialPalette.defaultRgb(MaterialPalette.GIRDER), BOLT_CHAMBER);
        for (int x = 16; x <= 24; x++)
            g.set(x, 4, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 17; x <= 20; x++)
            for (int z = cz - 1; z <= cz + 1; z++)
                g.set(x, 3, z, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), ACTION);
        for (int x = 16; x <= 26; x++) {
            g.set(x, 5, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
            g.set(x, 6, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
        }
        g.set(10, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
        g.set(24, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintMarksmanRifle(VoxelGrid g) {
        paintStarterRifle(g);
        int cz = g.sizeZ() / 2;
        // Extend barrel and add match optic
        for (int x = 19; x <= 26; x++)
            g.set(x, 4, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
        for (int x = 9; x <= 16; x++)
            g.set(x, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintLmgMedium(VoxelGrid g) {
        paintStarterRifle(g);
        int cz = g.sizeZ() / 2;
        // Heavy drum mag below action
        for (int x = 8; x <= 11; x++)
            for (int y = 0; y <= 2; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
    }

    public static void paintSniperHeavy(VoxelGrid g) {
        paintStarterRifle(g);
        int cz = g.sizeZ() / 2;
        for (int x = 19; x <= 28; x++)
            g.set(x, 4, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
        for (int x = 8; x <= 18; x++)
            g.set(x, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintCannonHeavy(VoxelGrid g) {
        g.clear();
        int cz = g.sizeZ() / 2;
        for (int x = 2; x <= 6; x++)
            for (int y = 1; y <= 4; y++)
                g.set(x, y, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), GRIP_STOCK);
        for (int x = 6; x <= 14; x++)
            for (int y = 3; y <= 7; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 14; x <= 22; x++)
            for (int y = 4; y <= 6; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), BARREL);
        g.set(10, 8, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintCarbineEnergy(VoxelGrid g) {
        g.clear();
        int cz = g.sizeZ() / 2;
        for (int x = 2; x <= 7; x++)
            for (int y = 2; y <= 5; y++)
                g.set(x, y, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), GRIP_STOCK);
        for (int x = 7; x <= 15; x++)
            for (int y = 3; y <= 6; y++)
                for (int z = cz - 1; z <= cz + 1; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 15; x <= 23; x++)
            g.set(x, 4, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), BARREL);
        g.set(11, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintRailgunEnergy(VoxelGrid g) {
        g.clear();
        int cz = g.sizeZ() / 2;
        for (int x = 2; x <= 7; x++)
            for (int y = 2; y <= 5; y++)
                g.set(x, y, cz, MaterialPalette.CARBON_FIBER, MaterialPalette.defaultRgb(MaterialPalette.CARBON_FIBER), GRIP_STOCK);
        for (int x = 7; x <= 15; x++)
            for (int y = 3; y <= 6; y++)
                g.set(x, y, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), ACTION);
        for (int x = 15; x <= 26; x++) {
            g.set(x, 5, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), BARREL);
            g.set(x, 3, cz, MaterialPalette.SHEET_METAL, MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), BARREL);
            g.set(x, 4, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), BARREL);
        }
        g.set(11, 7, cz, MaterialPalette.PLEXIGLASS, MaterialPalette.defaultRgb(MaterialPalette.PLEXIGLASS), SIGHT);
    }

    public static void paintWeapon(VoxelGrid g, String id) {
        if (id == null) { paintStarterRifle(g); return; }
        switch (id) {
            case "sidearm_light" -> paintSidearmLight(g);
            case "smg_light" -> paintSmgLight(g);
            case "shotgun_light" -> paintShotgunLight(g);
            case "marksman_rifle" -> paintMarksmanRifle(g);
            case "lmg_medium" -> paintLmgMedium(g);
            case "sniper_heavy" -> paintSniperHeavy(g);
            case "cannon_heavy" -> paintCannonHeavy(g);
            case "carbine_energy" -> paintCarbineEnergy(g);
            case "railgun_energy" -> paintRailgunEnergy(g);
            default -> paintStarterRifle(g);
        }
    }

    public static void exportWeaponJson(Path path, String id, Stats stats, String ammoId) throws IOException {
        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        sb.append("  \"id\": \"").append(id).append("\",\n");
        sb.append("  \"unit\": 1,\n");
        sb.append("  \"voxel_size\": 0.001,\n");
        sb.append("  \"caliber\": \"").append(stats.caliber).append("\",\n");
        sb.append("  \"hitscan\": ").append(stats.hitscan ? 1 : 0).append(",\n");
        String fm = stats.fireMode == null ? "semi" : stats.fireMode;
        sb.append("  \"fire_mode\": \"").append(fm).append("\",\n");
        sb.append("  \"ammo_id\": \"").append(ammoId == null ? "" : ammoId).append("\",\n");
        sb.append("  \"stats\": {\n");
        sb.append(String.format(Locale.ROOT, "    \"damage\": %.3f,\n", stats.damage));
        sb.append(String.format(Locale.ROOT, "    \"impact\": %.3f,\n", stats.impact));
        sb.append(String.format(Locale.ROOT, "    \"recoil\": %.3f,\n", stats.recoil));
        sb.append(String.format(Locale.ROOT, "    \"handling\": %.3f,\n", stats.handling));
        sb.append(String.format(Locale.ROOT, "    \"weight\": %.3f,\n", stats.weight));
        sb.append(String.format(Locale.ROOT, "    \"optic\": %.3f\n", stats.optic));
        sb.append("  },\n");
        sb.append("  \"parts\": {\n");
        boolean first = true;
        for (Map.Entry<String, Integer> e : stats.partCounts.entrySet()) {
            if (!first) sb.append(",\n");
            first = false;
            sb.append("    \"").append(e.getKey()).append("\": ").append(e.getValue());
        }
        sb.append("\n  }\n}\n");
        Files.createDirectories(path.getParent() == null ? Path.of(".") : path.getParent());
        Files.writeString(path, sb.toString(), StandardCharsets.UTF_8);
    }
}
