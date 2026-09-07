package voxel.painter.grid;

import java.util.ArrayDeque;
import java.util.Deque;

/** Grid-native paint tools. Stretch is integer unit-scale only. */
public final class PaintTools {
    public enum BrushShape { POINT, CUBE, SPHERE, LINE }

    public int matA = MaterialPalette.CONCRETE;
    public int rgbA = MaterialPalette.defaultRgb(MaterialPalette.CONCRETE);
    public int matB = MaterialPalette.WOOD;
    public int rgbB = MaterialPalette.defaultRgb(MaterialPalette.WOOD);
    public boolean useB;
    public BrushShape shape = BrushShape.CUBE;
    public int brushSize = 1; // odd preferred: 1,3,5

    public int activeMat() { return useB ? matB : matA; }
    public int activeRgb() { return useB ? rgbB : rgbA; }

    public void toggleAlternate() { useB = !useB; }

    public void paintAt(VoxelGrid g, int cx, int cy, int cz) {
        g.assertCubicUnitInvariant();
        int mat = activeMat();
        int rgb = activeRgb();
        int r = Math.max(0, brushSize / 2);
        switch (shape) {
            case POINT -> g.set(cx, cy, cz, mat, rgb);
            case CUBE -> {
                for (int z = cz - r; z <= cz + r; z++)
                    for (int y = cy - r; y <= cy + r; y++)
                        for (int x = cx - r; x <= cx + r; x++)
                            g.set(x, y, z, mat, rgb);
            }
            case SPHERE -> {
                int r2 = r * r;
                for (int z = cz - r; z <= cz + r; z++)
                    for (int y = cy - r; y <= cy + r; y++)
                        for (int x = cx - r; x <= cx + r; x++) {
                            int dx = x - cx, dy = y - cy, dz = z - cz;
                            if (dx * dx + dy * dy + dz * dz <= r2)
                                g.set(x, y, z, mat, rgb);
                        }
            }
            case LINE -> g.set(cx, cy, cz, mat, rgb); // single step; use paintLine for spans
        }
    }

    public void paintLine(VoxelGrid g, int x0, int y0, int z0, int x1, int y1, int z1) {
        int dx = Math.abs(x1 - x0), dy = Math.abs(y1 - y0), dz = Math.abs(z1 - z0);
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int sz = z0 < z1 ? 1 : -1;
        int dm = Math.max(dx, Math.max(dy, dz));
        if (dm == 0) {
            paintAt(g, x0, y0, z0);
            return;
        }
        for (int i = 0; i <= dm; i++) {
            int x = x0 + (dx == 0 ? 0 : (i * (x1 - x0)) / dm);
            int y = y0 + (dy == 0 ? 0 : (i * (y1 - y0)) / dm);
            int z = z0 + (dz == 0 ? 0 : (i * (z1 - z0)) / dm);
            paintAt(g, x, y, z);
        }
    }

    /** 6-connected flood fill replacing target mat (or air). */
    public void floodFill(VoxelGrid g, int sx, int sy, int sz) {
        if (!g.inBounds(sx, sy, sz)) return;
        int target = g.getMat(sx, sy, sz);
        int mat = activeMat();
        int rgb = activeRgb();
        if (target == mat && g.getRgb(sx, sy, sz) == rgb) return;
        Deque<int[]> q = new ArrayDeque<>();
        q.add(new int[]{sx, sy, sz});
        boolean[] seen = new boolean[g.sizeX() * g.sizeY() * g.sizeZ()];
        int[][] dirs = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
        while (!q.isEmpty()) {
            int[] p = q.removeFirst();
            int x = p[0], y = p[1], z = p[2];
            if (!g.inBounds(x, y, z)) continue;
            int id = (y * g.sizeZ() + z) * g.sizeX() + x;
            if (seen[id]) continue;
            if (g.getMat(x, y, z) != target) continue;
            seen[id] = true;
            g.set(x, y, z, mat, rgb);
            for (int[] d : dirs) q.add(new int[]{x + d[0], y + d[1], z + d[2]});
        }
    }

    /**
     * Integer stretch of inclusive box [x0..x1] etc by factors fx,fy,fz.
     * Factors must be >= 1; result remains unit cubes (each source cell maps to fx*fy*fz unit cells).
     * Placement origin stays at (x0,y0,z0).
     */
    public static void stretch(VoxelGrid g, int x0, int y0, int z0, int x1, int y1, int z1,
                               int fx, int fy, int fz) {
        if (fx < 1 || fy < 1 || fz < 1) throw new IllegalArgumentException("stretch factors >= 1");
        // Unified unit: stretching creates more unit cells, never non-cubic cells.
        g.assertCubicUnitInvariant();
        if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { int t = y0; y0 = y1; y1 = t; }
        if (z1 < z0) { int t = z0; z0 = z1; z1 = t; }
        VoxelGrid snap = g.copy();
        // clear region that will be rewritten (expanded)
        int nx1 = x0 + (x1 - x0 + 1) * fx - 1;
        int ny1 = y0 + (y1 - y0 + 1) * fy - 1;
        int nz1 = z0 + (z1 - z0 + 1) * fz - 1;
        for (int z = z0; z <= nz1; z++)
            for (int y = y0; y <= ny1; y++)
                for (int x = x0; x <= nx1; x++)
                    g.set(x, y, z, MaterialPalette.AIR, 0);
        for (int z = z0; z <= z1; z++)
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    int m = snap.getMat(x, y, z);
                    int c = snap.getRgb(x, y, z);
                    if (m == MaterialPalette.AIR) continue;
                    int bx = x0 + (x - x0) * fx;
                    int by = y0 + (y - y0) * fy;
                    int bz = z0 + (z - z0) * fz;
                    for (int dz = 0; dz < fz; dz++)
                        for (int dy = 0; dy < fy; dy++)
                            for (int dx = 0; dx < fx; dx++)
                                g.set(bx + dx, by + dy, bz + dz, m, c);
                }
    }

    public void dropper(VoxelGrid g, int x, int y, int z) {
        int m = g.getMat(x, y, z);
        int c = g.getRgb(x, y, z);
        if (c == 0) c = MaterialPalette.defaultRgb(m);
        if (useB) { matB = m; rgbB = c; }
        else { matA = m; rgbA = c; }
    }

    /** F-gradient: sample between colorA and colorB at t in [0,1], set active RGB (custom mat). */
    public void sampleFGradient(int rgb0, int rgb1, float t) {
        t = Math.max(0f, Math.min(1f, t));
        int r0 = (rgb0 >> 16) & 0xFF, g0 = (rgb0 >> 8) & 0xFF, b0 = rgb0 & 0xFF;
        int r1 = (rgb1 >> 16) & 0xFF, g1 = (rgb1 >> 8) & 0xFF, b1 = rgb1 & 0xFF;
        int r = Math.round(r0 + (r1 - r0) * t);
        int g = Math.round(g0 + (g1 - g0) * t);
        int b = Math.round(b0 + (b1 - b0) * t);
        int rgb = (r << 16) | (g << 8) | b;
        if (useB) { matB = MaterialPalette.CUSTOM; rgbB = rgb; }
        else { matA = MaterialPalette.CUSTOM; rgbA = rgb; }
    }
}
