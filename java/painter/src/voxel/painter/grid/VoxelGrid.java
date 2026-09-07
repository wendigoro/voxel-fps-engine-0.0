package voxel.painter.grid;

import java.util.Arrays;

/**
 * Dense cubic unit voxel occupancy grid.
 * Every cell is a 1x1x1 unit cube; world edge = VOXEL_SIZE (0.001).
 * Optional weapon part labels per cell (0=none).
 */
public final class VoxelGrid {
    public static final float VOXEL_SIZE = 0.001f;
    public static final int UNIT = 1;

    private final int sizeX, sizeY, sizeZ;
    private final byte[] mat;
    private final int[] rgb;
    private final byte[] part; // WeaponPart id

    public VoxelGrid(int sizeX, int sizeY, int sizeZ) {
        if (sizeX < 1 || sizeY < 1 || sizeZ < 1) throw new IllegalArgumentException("dims");
        if (UNIT != 1) throw new IllegalStateException("UNIT must be 1");
        this.sizeX = sizeX; this.sizeY = sizeY; this.sizeZ = sizeZ;
        int n = sizeX * sizeY * sizeZ;
        this.mat = new byte[n];
        this.rgb = new int[n];
        this.part = new byte[n];
    }

    public int sizeX() { return sizeX; }
    public int sizeY() { return sizeY; }
    public int sizeZ() { return sizeZ; }
    public int unitSizeX() { return UNIT; }
    public int unitSizeY() { return UNIT; }
    public int unitSizeZ() { return UNIT; }

    public void assertCubicUnitInvariant() {
        if (unitSizeX() != unitSizeY() || unitSizeY() != unitSizeZ())
            throw new IllegalStateException("non-cubic unit size");
        if (unitSizeX() != UNIT) throw new IllegalStateException("unit != 1");
    }

    private int idx(int x, int y, int z) { return (y * sizeZ + z) * sizeX + x; }

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
    public int getPart(int x, int y, int z) {
        if (!inBounds(x, y, z)) return 0;
        return part[idx(x, y, z)] & 0xFF;
    }

    public void set(int x, int y, int z, int materialId, int rgb24) {
        set(x, y, z, materialId, rgb24, getPart(x, y, z));
    }
    public void set(int x, int y, int z, int materialId, int rgb24, int partId) {
        if (!inBounds(x, y, z)) return;
        assertCubicUnitInvariant();
        int i = idx(x, y, z);
        mat[i] = (byte) (materialId & 0xFF);
        rgb[i] = rgb24 & 0xFFFFFF;
        part[i] = (byte) (partId & 0xFF);
    }
    public void setMat(int x, int y, int z, int materialId) {
        set(x, y, z, materialId, MaterialPalette.defaultRgb(materialId), getPart(x, y, z));
    }
    public void setPart(int x, int y, int z, int partId) {
        if (!inBounds(x, y, z)) return;
        part[idx(x, y, z)] = (byte) (partId & 0xFF);
    }

    public void clear() {
        Arrays.fill(mat, (byte) 0);
        Arrays.fill(rgb, 0);
        Arrays.fill(part, (byte) 0);
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
        System.arraycopy(part, 0, g.part, 0, part.length);
        return g;
    }
}
