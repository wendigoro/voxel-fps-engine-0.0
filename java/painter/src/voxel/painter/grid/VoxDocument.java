package voxel.painter.grid;

import java.util.Locale;

/** In-memory .vox.json document. */
public final class VoxDocument {
    public enum Mode { MODEL, SKY, CHARACTER }

    public final int unit = VoxelGrid.UNIT;
    public final float voxelSize = VoxelGrid.VOXEL_SIZE;
    public Mode mode = Mode.MODEL;
    public VoxelGrid grid;
    public int segU = 28;
    public int segV = 14;
    public float moonDirX = 0.32f, moonDirY = 0.82f, moonDirZ = -0.48f;
    public float moonIntensity = 0.95f;
    public int feetX, feetY, feetZ;

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
            default -> Mode.MODEL;
        };
    }

    public String modeName() {
        return switch (mode) {
            case SKY -> "sky";
            case CHARACTER -> "character";
            default -> "model";
        };
    }
}
