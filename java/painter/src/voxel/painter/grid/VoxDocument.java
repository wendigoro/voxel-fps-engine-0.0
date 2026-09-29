package voxel.painter.grid;

import java.util.Locale;

/** In-memory .vox.json document. */
public final class VoxDocument {
public enum Mode { MODEL, SKY, CHARACTER, WEAPON, ITEM, MAP }

    public final int unit = VoxelGrid.UNIT;
    public final float voxelSize = VoxelGrid.VOXEL_SIZE;
    public Mode mode = Mode.MODEL;
    public VoxelGrid grid;
    public int segU = 28;
    public int segV = 14;
    public float moonDirX = 0.32f, moonDirY = 0.82f, moonDirZ = -0.48f;
    public float moonIntensity = 0.95f;
public int feetX, feetY, feetZ;
    public String caliber = "medium"; // light|medium|heavy|energy
    public String ammoId = "medium_fmj";
    public String fireMode = "semi"; // semi|auto|bolt
    // Item mode metadata. Only meaningful when mode == ITEM; exported to
    // *.item.json for the engine's inventory (RULES.md rule 12). The grid is the
    // item's own packing shape in unit cubes, so its extent IS the "size" field.
    public String itemId = "item_new";
    public String itemName = "New Item";
    public String itemClass = "misc"; // see Items.CLASSES
    public String armorZone = "";     // head|chest|arms|legs (armor only)
    public int packSX, packSY, packSZ; // granted storage volume (backpack only)
    // Map mode metadata: scripted events, NPCs and patrol routes. The grid is
    // the map itself, still on the cubic unit grid. Entity coordinates are
    // integer cell indices, never world-space floats.
    public MapEntities.MapData mapData = new MapEntities.MapData();

    public VoxDocument(Mode mode, int sx, int sy, int sz) {
        this.mode = mode;
        this.grid = new VoxelGrid(sx, sy, sz);
        this.grid.assertCubicUnitInvariant();
    }

    public static Mode parseMode(String s) {
        if (s == null) return Mode.MODEL;
return switch (s.toLowerCase(Locale.ROOT)) {
            case "sky" -> Mode.SKY;
            case "character" -> Mode.CHARACTER;
            case "weapon" -> Mode.WEAPON;
            case "item" -> Mode.ITEM;
            case "map" -> Mode.MAP;
            default -> Mode.MODEL;
        };
    }

    public String modeName() {
return switch (mode) {
            case SKY -> "sky";
            case CHARACTER -> "character";
            case WEAPON -> "weapon";
            case ITEM -> "item";
            case MAP -> "map";
            default -> "model";
        };
    }
}
