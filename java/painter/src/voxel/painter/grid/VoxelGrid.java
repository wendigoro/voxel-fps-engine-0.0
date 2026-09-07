package voxel.painter.grid;

import java.util.Arrays;

/**
 * Dense cubic unit voxel occupancy grid.
 * Every cell is a 1x1x1 unit cube; world edge = VOXEL_SIZE (0.001).
 * Cell size on Z is always the same unit as X and Y.
 */
public final class VoxelGrid {
    public static final float VOXEL_SIZE = 0.001f;
    public static final int UNIT = 1; // integer unit cube edge

    private final int sizeX;
    private final int sizeY;
    private final int sizeZ;
    private final byte[] mat;   // material id
    private final int[] rgb;    // 0xRRGGBB (used for sky/custom)

    public VoxelGrid(int sizeX, int sizeY, int sizeZ) {
        if (sizeX < 1 || sizeY < 1 || sizeZ < 1) {
            throw new IllegalArgumentException("dims must be >= 1");
        }
        // Cubic unit invariant: cell extent is always UNIT on all axes.
        if (UNIT != 1) throw new IllegalStateException("UNIT must be 1");
        this.sizeX = sizeX;
        this.sizeY = sizeY;
        this.sizeZ = sizeZ;
        int n = sizeX * sizeY * sizeZ;
        this.mat = new byte[n];
        this.rgb = new int[n];
        Arrays.fill(this.rgb, 0);
    }

    public int sizeX() { return sizeX; }
    public int sizeY() { return sizeY; }
    public int sizeZ() { return sizeZ; }

    /** Cell edge length in unit voxels — always equal on X, Y, Z. */
    public int unitSizeX() { return UNIT; }
    public int unitSizeY() { return UNIT; }
    public int unitSizeZ() { return UNIT; }

    public void assertCubicUnitInvariant() {
        if (unitSizeX() != unitSizeY() || unitSizeY() != unitSizeZ()) {
            throw new IllegalStateException("non-cubic unit size");
        }
        if (unitSizeX() != UNIT) throw new IllegalStateException("unit != 1");
    }

    private int idx(int x, int y, int z) {
        return (y * sizeZ + z) * sizeX + x;
    }

    public boolean inBounds(int x, int y, int z) {
        return x >= 0 && y >= 0 && z >= 0 && x < sizeX && y < sizeY && z < sizeZ;
    }

    public int getMat(int x, int y, int z) {
        if (!inBounds(x, y, z)) return MaterialPalette.AIR;
        return mat[idx(x, y, z)] & 0xFF;
    }

    public int getRgb(int x, int y, int z) {
        if (!inBounds(x, y, z)) return 0;
        return rgb[idx(x, y, z)] & 0xFFFFFF;
    }

    public void set(int x, int y, int z, int materialId, int rgb24) {
        if (!inBounds(x, y, z)) return;
        assertCubicUnitInvariant();
        int i = idx(x, y, z);
        mat[i] = (byte) (materialId & 0xFF);
        rgb[i] = rgb24 & 0xFFFFFF;
    }

    public void setMat(int x, int y, int z, int materialId) {
        set(x, y, z, materialId, MaterialPalette.defaultRgb(materialId));
    }

    public void clear() {
        Arrays.fill(mat, (byte) 0);
        Arrays.fill(rgb, 0);
    }

    public int solidCount() {
        int c = 0;
        for (byte b : mat) if ((b & 0xFF) != MaterialPalette.AIR) c++;
        return c;
    }

    public VoxelGrid copy() {
        VoxelGrid g = new VoxelGrid(sizeX, sizeY, sizeZ);
        System.arraycopy(mat, 0, g.mat, 0, mat.length);
        System.arraycopy(rgb, 0, g.rgb, 0, rgb.length);
        return g;
    }
}
