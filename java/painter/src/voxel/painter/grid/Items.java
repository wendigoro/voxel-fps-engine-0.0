package voxel.painter.grid;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;

/**
 * Inventory item definition + JSON export (RULES.md rule 12).
 *
 * <p>An item is a cubic-unit voxel shape. The document grid is only a work area:
 * the item's storage footprint is the tight bounding box of the solid cells the
 * artist painted, expressed in whole unit cells. There is deliberately no separate
 * scale control — stretching an item's storage footprint is exactly the
 * divergence rule 12 forbids.
 *
 * <p>Weapon magazines and pouch round counts stay metadata only. Nothing in this
 * class feeds the firing or reload path.
 */
public final class Items {
    private Items() {}

    public static final String CLASS_WEAPON_PRIMARY = "weapon_primary";
    public static final String CLASS_WEAPON_SMALL = "weapon_small";
    public static final String CLASS_AMMO_POUCH = "ammo_pouch";
    public static final String CLASS_ARMOR = "armor";
    public static final String CLASS_BACKPACK = "backpack";
    public static final String CLASS_MISC = "misc";

    public static final String[] CLASSES = {
        CLASS_WEAPON_PRIMARY, CLASS_WEAPON_SMALL, CLASS_AMMO_POUCH,
        CLASS_ARMOR, CLASS_BACKPACK, CLASS_MISC
    };

    public static final String[] ARMOR_ZONES = { "head", "chest", "arms", "legs" };

    /** True when the class requires an armor_zone to be equippable. */
    public static boolean needsArmorZone(String cls) {
        return CLASS_ARMOR.equals(cls);
    }

    /** True when the class requires pack_size to grant storage. */
    public static boolean needsPackSize(String cls) {
        return CLASS_BACKPACK.equals(cls);
    }

    /** Resolve the export file name for an item id. */
    public static String fileNameFor(String id) {
        return sanitizeId(id) + ".item.json";
    }

    /**
     * Item ids become file names, so restrict them to characters that are safe on
     * every filesystem we ship to. Returns "item" for an id that sanitises empty.
     */
    public static String sanitizeId(String id) {
        if (id == null) return "item";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < id.length() && sb.length() < 48; i++) {
            char c = id.charAt(i);
            boolean ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
            sb.append(ok ? c : '_');
        }
        return sb.length() == 0 ? "item" : sb.toString();
    }

    /**
     * Average tint of the painted voxels, used for the item's flat colour. The
     * engine tints inventory cells uniformly, so the average is what the player
     * actually sees. Returns null when the grid is empty.
     */
    public static float[] averageRgb(VoxelGrid g) {
        g.assertCubicUnitInvariant();
        long r = 0, gg = 0, b = 0;
        int n = 0;
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++) {
                    if (g.getMat(x, y, z) == MaterialPalette.AIR) continue;
                    int rgb = g.getRgb(x, y, z);
                    r += (rgb >> 16) & 0xFF;
                    gg += (rgb >> 8) & 0xFF;
                    b += rgb & 0xFF;
                    n++;
                }
        if (n == 0) return null;
        return new float[]{ r / (float) n / 255f, gg / (float) n / 255f, b / (float) n / 255f };
    }

    /** The material name that dominates the painted voxels, or sheet_metal. */
    public static String dominantMaterial(VoxelGrid g) {
        g.assertCubicUnitInvariant();
        int[] counts = new int[MaterialPalette.count()];
        int n = 0;
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++) {
                    int m = g.getMat(x, y, z);
                    if (m == MaterialPalette.AIR) continue;
                    if (m >= 0 && m < counts.length) counts[m]++;
                    n++;
                }
        if (n == 0) return "sheet_metal";
        int best = MaterialPalette.SHEET_METAL;
        for (int m = 1; m < counts.length; m++)
            if (counts[m] > counts[best]) best = m;
        return MaterialPalette.nameFromId(best);
    }

    /** Count of non-air cells, i.e. the item's storage cost. */
    public static int solidCells(VoxelGrid g) {
        int n = 0;
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++)
                    if (g.getMat(x, y, z) != MaterialPalette.AIR) n++;
        return n;
    }

    /**
     * Tight extent of the painted solid cells, in whole unit cells: the size the
     * engine will actually charge to store this item. Returns null when nothing is
     * painted. The canvas may be larger than this; the padding is not carried into
     * the item.
     */
    public static int[] footprintSize(VoxelGrid g) {
        int minX = Integer.MAX_VALUE, minY = Integer.MAX_VALUE, minZ = Integer.MAX_VALUE;
        int maxX = Integer.MIN_VALUE, maxY = Integer.MIN_VALUE, maxZ = Integer.MIN_VALUE;
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++) {
                    if (g.getMat(x, y, z) == MaterialPalette.AIR) continue;
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (z < minZ) minZ = z;
                    if (x > maxX) maxX = x;
                    if (y > maxY) maxY = y;
                    if (z > maxZ) maxZ = z;
                }
        if (maxX < minX) return null;
        return new int[]{ maxX - minX + 1, maxY - minY + 1, maxZ - minZ + 1 };
    }

    private static String esc(String s) {
        if (s == null) return "";
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') sb.append('\\');
            sb.append(c);
        }
        return sb.toString();
    }

    /**
     * Write the item definition the engine's inventory loads. {@code size} is the
     * tight bounding box of the painted solid cells and {@code cells} lists them
     * rebased to that origin, so an item keeps its exact footprint and never pays
     * for the empty canvas around it.
     */
    public static void exportItemJson(Path path, VoxDocument doc) throws IOException {
        if (doc.mode != VoxDocument.Mode.ITEM)
            throw new IllegalArgumentException("exportItemJson requires Mode.ITEM, got " + doc.mode);
        VoxelGrid g = doc.grid;
        g.assertCubicUnitInvariant();
        int[] fp = footprintSize(g);
        if (fp == null)
            throw new IllegalArgumentException("item '" + doc.itemId + "' has no solid cells");
        if (fp[0] > 32 || fp[1] > 32 || fp[2] > 32)
            throw new IllegalArgumentException("item footprint " + fp[0] + "x" + fp[1] + "x"
                    + fp[2] + " exceeds 32 unit cells per axis");

        float[] rgb = averageRgb(g);
        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        sb.append("  \"id\": \"").append(esc(sanitizeId(doc.itemId))).append("\",\n");
        sb.append("  \"name\": \"").append(esc(doc.itemName)).append("\",\n");
        sb.append("  \"unit\": ").append(doc.unit).append(",\n");
        sb.append("  \"voxel_size\": ").append(doc.voxelSize).append(",\n");
        sb.append("  \"class\": \"").append(esc(doc.itemClass)).append("\",\n");
        if (needsArmorZone(doc.itemClass) && doc.armorZone != null && !doc.armorZone.isEmpty())
            sb.append("  \"armor_zone\": \"").append(esc(doc.armorZone)).append("\",\n");
        sb.append("  \"material\": \"").append(esc(dominantMaterial(g))).append("\",\n");
        sb.append(String.format(Locale.ROOT, "  \"color\": [%.3f, %.3f, %.3f],\n", rgb[0], rgb[1], rgb[2]));
        sb.append("  \"size\": [").append(fp[0]).append(", ").append(fp[1])
                .append(", ").append(fp[2]).append("],\n");
        // Flattened [x,y,z] triples, rebased to the footprint origin, so the engine
        // rebuilds the authored shape rather than a solid bounding box.
        int minX = Integer.MAX_VALUE, minY = Integer.MAX_VALUE, minZ = Integer.MAX_VALUE;
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++) {
                    if (g.getMat(x, y, z) == MaterialPalette.AIR) continue;
                    if (x < minX) minX = x;
                    if (y < minY) minY = y;
                    if (z < minZ) minZ = z;
                }
        StringBuilder cells = new StringBuilder();
        int written = 0;
        for (int y = minY; y < g.sizeY(); y++)
            for (int z = minZ; z < g.sizeZ(); z++)
                for (int x = minX; x < g.sizeX(); x++) {
                    if (g.getMat(x, y, z) == MaterialPalette.AIR) continue;
                    if (written++ > 0) cells.append(", ");
                    cells.append(x - minX).append(", ").append(y - minY).append(", ").append(z - minZ);
                }
        sb.append("  \"cells\": [").append(cells).append("],\n");
        if (needsPackSize(doc.itemClass) && doc.packSX > 0 && doc.packSY > 0 && doc.packSZ > 0)
            sb.append("  \"pack_size\": [").append(doc.packSX).append(", ").append(doc.packSY)
                    .append(", ").append(doc.packSZ).append("],\n");
        if (CLASS_WEAPON_PRIMARY.equals(doc.itemClass) || CLASS_WEAPON_SMALL.equals(doc.itemClass)) {
            sb.append("  \"weapon_id\": \"").append(esc(sanitizeId(doc.itemId))).append("\",\n");
            sb.append("  \"caliber\": \"").append(esc(doc.caliber)).append("\",\n");
            sb.append("  \"ammo_id\": \"").append(esc(doc.ammoId)).append("\",\n");
        } else if (CLASS_AMMO_POUCH.equals(doc.itemClass)) {
            sb.append("  \"caliber\": \"").append(esc(doc.caliber)).append("\",\n");
            sb.append("  \"ammo_id\": \"").append(esc(doc.ammoId)).append("\",\n");
        }
        // Strip the trailing comma from the final field.
        String out = sb.toString();
        out = out.substring(0, out.length() - 2) + "\n}\n";
        Files.createDirectories(path.getParent() == null ? Path.of(".") : path.getParent());
        Files.writeString(path, out, StandardCharsets.UTF_8);
    }

    /** Minimal cubic-unit starter templates so item mode is not a blank grid. */
    public static void paintStarterPouch(VoxelGrid g) {
        g.clear();
        for (int x = 0; x < 2; x++)
            for (int y = 0; y < 2; y++)
                for (int z = 0; z < 2; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL,
                            MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), 0);
    }

    public static void paintStarterPlate(VoxelGrid g) {
        g.clear();
        for (int x = 0; x < 3; x++)
            for (int y = 0; y < 3; y++)
                for (int z = 0; z < 3; z++)
                    g.set(x, y, z, MaterialPalette.SHEET_METAL,
                            MaterialPalette.defaultRgb(MaterialPalette.SHEET_METAL), 0);
    }
}
