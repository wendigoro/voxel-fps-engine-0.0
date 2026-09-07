package voxel.painter.grid;

/** Sky hemisphere tile bake + character unit-voxel template. */
public final class SkyAndCharacter {
    private SkyAndCharacter() {}

    public static float hash21(int x, int y) {
        int n = x * 374761393 + y * 668265263;
        n = (n ^ (n >>> 13)) * 1274126177;
        n ^= n >>> 16;
        return (n & 0xFFFF) / 65535f;
    }

    /**
     * Fills a 2D sky tile sheet into grid Y=0 plane as unit cells (x=u, z=v).
     * Matches engine-style seg grid: elev gradient, stars, moon wash.
     */
    public static void paintSkyTiles(VoxelGrid g, int segU, int segV,
                                     float moonX, float moonY, float moonZ,
                                     float moonIntensity) {
        g.assertCubicUnitInvariant();
        g.clear();
        // normalize moon
        float mlen = (float) Math.sqrt(moonX * moonX + moonY * moonY + moonZ * moonZ);
        if (mlen < 1e-6f) { moonX = 0.32f; moonY = 0.82f; moonZ = -0.48f; mlen = 1f; }
        moonX /= mlen; moonY /= mlen; moonZ /= mlen;

        for (int v = 0; v < segV && v < g.sizeZ(); v++) {
            float elev = (v + 0.5f) / segV;
            for (int u = 0; u < segU && u < g.sizeX(); u++) {
                float az = (u + 0.5f) / segU * (float) (Math.PI * 2.0);
                float el = (0.08f + elev * 0.92f) * (float) (Math.PI * 0.5);
                float ce = (float) Math.cos(el);
                float dx = (float) Math.cos(az) * ce;
                float dy = (float) Math.sin(el);
                float dz = (float) Math.sin(az) * ce;

                // night gradient
                int zr = 5, zg = 8, zb = 20;
                int hr = 20, hg = 18, hb = 30;
                int r = Math.round(hr + (zr - hr) * elev);
                int gr = Math.round(hg + (zg - hg) * elev);
                int b = Math.round(hb + (zb - hb) * elev);

                // stars
                if (elev > 0.35f && hash21(u * 3, v * 7) > 0.97f) {
                    r = 220; gr = 230; b = 255;
                }

                // moon facing wash
                float facing = Math.max(0f, dx * moonX + dy * moonY + dz * moonZ);
                facing = (float) Math.pow(facing, 8.0);
                r = clamp255(r + (int) (moonIntensity * 90 * facing) + (int) (moonIntensity * 8));
                gr = clamp255(gr + (int) (moonIntensity * 100 * facing) + (int) (moonIntensity * 10));
                b = clamp255(b + (int) (moonIntensity * 140 * facing) + (int) (moonIntensity * 14));

                // pixel quantize slightly
                r = (r / 18) * 18; gr = (gr / 18) * 18; b = (b / 18) * 18;
                int rgb = (r << 16) | (gr << 8) | b;
                g.set(u, 0, v, MaterialPalette.CUSTOM, rgb);
            }
        }
        // moon sprite disk on sky sheet near bearing
        int mu = Math.floorMod(Math.round(azToU(moonX, moonZ, segU)), segU);
        int mv = Math.max(0, Math.min(segV - 1, Math.round(elevToV(moonY, segV))));
        paintMoonSprite(g, mu, mv, segU, segV);
    }

    private static int azToU(float mx, float mz, int segU) {
        double az = Math.atan2(mz, mx);
        if (az < 0) az += Math.PI * 2;
        return (int) Math.round(az / (Math.PI * 2) * segU) % segU;
    }

    private static int elevToV(float my, int segV) {
        double el = Math.asin(Math.max(-1, Math.min(1, my)));
        double t = (el / (Math.PI * 0.5) - 0.08) / 0.92;
        return (int) Math.round(Math.max(0, Math.min(1, t)) * (segV - 1));
    }

    private static void paintMoonSprite(VoxelGrid g, int cu, int cv, int segU, int segV) {
        int rad = 2;
        for (int dv = -rad; dv <= rad; dv++) {
            for (int du = -rad; du <= rad; du++) {
                float d1 = (float) Math.sqrt(du * du + dv * dv);
                if (d1 > rad + 0.2f) continue;
                float d2 = (float) Math.sqrt((du - 1.2f) * (du - 1.2f) + (dv - 0.2f) * (dv - 0.2f));
                float disk = d1 < rad * 0.85f ? 1f : 0f;
                float cut = d2 < rad * 0.75f ? 1f : 0f;
                float crescent = disk * (1f - cut * 0.85f);
                if (crescent < 0.2f && d1 > rad * 0.5f) continue;
                int u = Math.floorMod(cu + du, Math.max(1, Math.min(segU, g.sizeX())));
                int v = cv + dv;
                if (v < 0 || v >= Math.min(segV, g.sizeZ())) continue;
                int rgb = crescent > 0.35f ? 0xE0E8FF : 0x6A80B8;
                g.set(u, 0, v, MaterialPalette.CUSTOM, rgb);
            }
        }
    }

    private static int clamp255(int v) {
        return Math.max(0, Math.min(255, v));
    }

    /** Unit-voxel humanoid standing at feet (fx,fy,fz). All cells 1x1x1. */
    public static void paintCharacter(VoxelGrid g, int fx, int fy, int fz) {
        g.assertCubicUnitInvariant();
        int[][] units = {
            // legs
            {0,0,0},{1,0,0},{0,1,0},{1,1,0},{3,0,0},{4,0,0},{3,1,0},{4,1,0},
            // torso
            {0,2,0},{1,2,0},{2,2,0},{3,2,0},{4,2,0},
            {0,3,0},{1,3,0},{2,3,0},{3,3,0},{4,3,0},
            {0,4,0},{1,4,0},{2,4,0},{3,4,0},{4,4,0},
            // head
            {1,5,0},{2,5,0},{3,5,0},{1,6,0},{2,6,0},{3,6,0},
            // arms
            {-1,3,0},{-1,4,0},{5,3,0},{5,4,0}
        };
        for (int[] u : units) {
            int x = fx + u[0], y = fy + u[1], z = fz + u[2];
            int mat;
            int yrel = u[1];
            if (yrel <= 1) mat = MaterialPalette.GIRDER; // pants stand-in
            else if (yrel <= 4) mat = MaterialPalette.SHEET_METAL; // shirt
            else mat = MaterialPalette.CONCRETE; // head/skin stand-in
            if (u[0] < 0 || u[0] > 4) mat = MaterialPalette.CONCRETE; // arms
            g.setMat(x, y, z, mat);
        }
        // accent belt
        g.setMat(fx + 2, fy + 3, fz, MaterialPalette.WOOD);
    }
}
