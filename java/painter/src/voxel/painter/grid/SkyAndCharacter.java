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

    /** Unit-voxel humanoid with solid segment volumes for impact/penetration.
     * Segments: head, neck, torso, pelvis, upper_arms, lower_arms, upper_legs, lower_legs.
     * Each segment has 1-2 voxel thick walls + solid interior (1-2 voxels deep).
     * Uses CHARACTER_BONE (breakable, non-reflecting) and CHARACTER_FLESH (low density, high damping). */
    public static void paintCharacter(VoxelGrid g, int fx, int fy, int fz) {
        g.assertCubicUnitInvariant();
        // Segment definitions: [x0,y0,z0, x1,y1,z1, material]
        // Y-up from feet. All coords relative to (fx, fy, fz).
        int[][] segments = {
            // head: 3x3x3 box at y=6..8, x=1..3, z=-1..1 (centered)
            {1, 6, -1, 3, 8, 1, MaterialPalette.CHARACTER_BONE},      // skull outer
            {2, 7, 0, 2, 7, 0, MaterialPalette.CHARACTER_FLESH},      // brain cavity

            // neck: 1x2x1 at y=5..6, x=2, z=0
            {2, 5, 0, 2, 6, 0, MaterialPalette.CHARACTER_BONE},

            // torso: 5x7x3 box at y=2..8, x=0..4, z=-1..1
            {0, 2, -1, 4, 8, 1, MaterialPalette.CHARACTER_BONE},      // ribcage shell
            {1, 3, 0, 3, 7, 0, MaterialPalette.CHARACTER_FLESH},      // organ cavity

            // pelvis: 5x3x3 at y=0..2, x=0..4, z=-1..1
            {0, 0, -1, 4, 2, 1, MaterialPalette.CHARACTER_BONE},      // pelvic bone shell
            {1, 1, 0, 3, 1, 0, MaterialPalette.CHARACTER_FLESH},      // pelvic cavity

            // upper arms (shoulders to elbows): 2x4x2 at y=3..6
            {-2, 3, -1, -1, 6, 0, MaterialPalette.CHARACTER_BONE},    // left upper arm shell
            {5, 3, -1, 6, 6, 0, MaterialPalette.CHARACTER_BONE},      // right upper arm shell
            {-2, 4, 0, -1, 5, 0, MaterialPalette.CHARACTER_FLESH},    // left upper arm interior
            {5, 4, 0, 6, 5, 0, MaterialPalette.CHARACTER_FLESH},      // right upper arm interior

            // lower arms (forearms): 2x3x2 at y=0..2 (attached at elbow y=3)
            {-2, 0, -1, -1, 2, 0, MaterialPalette.CHARACTER_BONE},    // left forearm shell
            {5, 0, -1, 6, 2, 0, MaterialPalette.CHARACTER_BONE},      // right forearm shell
            {-2, 1, 0, -1, 1, 0, MaterialPalette.CHARACTER_FLESH},    // left forearm interior
            {5, 1, 0, 6, 1, 0, MaterialPalette.CHARACTER_FLESH},      // right forearm interior

            // upper legs (thighs): 3x5x2 at y=0..4 (attached at hip y=2)
            {0, 0, -1, 2, 4, 0, MaterialPalette.CHARACTER_BONE},      // left thigh shell
            {2, 0, -1, 4, 4, 0, MaterialPalette.CHARACTER_BONE},      // right thigh shell
            {1, 1, 0, 1, 3, 0, MaterialPalette.CHARACTER_FLESH},      // left thigh interior
            {3, 1, 0, 3, 3, 0, MaterialPalette.CHARACTER_FLESH},      // right thigh interior

            // lower legs (calves): 2x4x2 at y=-3..0 (below feet at y=0)
            {0, -3, -1, 1, 0, 0, MaterialPalette.CHARACTER_BONE},     // left calf shell
            {3, -3, -1, 4, 0, 0, MaterialPalette.CHARACTER_BONE},     // right calf shell
            {0, -2, 0, 1, -1, 0, MaterialPalette.CHARACTER_FLESH},    // left calf interior
            {3, -2, 0, 4, -1, 0, MaterialPalette.CHARACTER_FLESH},    // right calf interior
        };

        for (int[] s : segments) {
            int x0 = fx + s[0], y0 = fy + s[1], z0 = fz + s[2];
            int x1 = fx + s[3], y1 = fy + s[4], z1 = fz + s[5];
            int mat = s[6];
            if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
            if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
            if (z0 > z1) { int t = z0; z0 = z1; z1 = t; }
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++)
                        g.setMat(x, y, z, mat);
        }
    }
}
