package voxel.painter.grid;

import java.util.ArrayList;
import java.util.List;

/**
 * Visual hitbox overlay for character cosmetic design.
 * Renders segment boundaries as wireframe boxes on a separate layer.
 */
public final class HitboxOverlay {
    private HitboxOverlay() {}

    /**
     * Segment definition matching SkyAndCharacter.paintCharacter.
     * Each segment: [x0,y0,z0, x1,y1,z1, materialId, label]
     */
    public static final class Segment {
        public final int x0, y0, z0, x1, y1, z1;
        public final int materialId;
        public final String label;
        public final int color; // ARGB for wireframe

        public Segment(int x0, int y0, int z0, int x1, int y1, int z1,
                       int materialId, String label, int color) {
            this.x0 = Math.min(x0, x1); this.y0 = Math.min(y0, y1); this.z0 = Math.min(z0, z1);
            this.x1 = Math.max(x0, x1); this.y1 = Math.max(y0, y1); this.z1 = Math.max(z0, z1);
            this.materialId = materialId;
            this.label = label;
            this.color = color;
        }
    }

    /** Default character hitbox segments (relative to feet position). */
    private static final Segment[] DEFAULT_SEGMENTS = {
        // Head group
        new Segment(1, 6, -1, 3, 8, 1, MaterialPalette.CHARACTER_BONE, "skull", 0xFFFF6600),
        new Segment(2, 7, 0, 2, 7, 0, MaterialPalette.CHARACTER_FLESH, "brain", 0xFFCC3300),

        // Neck
        new Segment(2, 5, 0, 2, 6, 0, MaterialPalette.CHARACTER_BONE, "neck", 0xFFFF6600),

        // Torso group
        new Segment(0, 2, -1, 4, 8, 1, MaterialPalette.CHARACTER_BONE, "ribcage", 0xFFFF6600),
        new Segment(1, 3, 0, 3, 7, 0, MaterialPalette.CHARACTER_FLESH, "organs", 0xFFCC3300),

        // Pelvis group
        new Segment(0, 0, -1, 4, 2, 1, MaterialPalette.CHARACTER_BONE, "pelvis", 0xFFFF6600),
        new Segment(1, 1, 0, 3, 1, 0, MaterialPalette.CHARACTER_FLESH, "pelvic_cavity", 0xFFCC3300),

        // Left upper arm
        new Segment(-2, 3, -1, -1, 6, 0, MaterialPalette.CHARACTER_BONE, "L_upper_arm_bone", 0xFFFF6600),
        new Segment(-2, 4, 0, -1, 5, 0, MaterialPalette.CHARACTER_FLESH, "L_upper_arm_flesh", 0xFFCC3300),

        // Right upper arm
        new Segment(5, 3, -1, 6, 6, 0, MaterialPalette.CHARACTER_BONE, "R_upper_arm_bone", 0xFFFF6600),
        new Segment(5, 4, 0, 6, 5, 0, MaterialPalette.CHARACTER_FLESH, "R_upper_arm_flesh", 0xFFCC3300),

        // Left lower arm
        new Segment(-2, 0, -1, -1, 2, 0, MaterialPalette.CHARACTER_BONE, "L_forearm_bone", 0xFFFF6600),
        new Segment(-2, 1, 0, -1, 1, 0, MaterialPalette.CHARACTER_FLESH, "L_forearm_flesh", 0xFFCC3300),

        // Right lower arm
        new Segment(5, 0, -1, 6, 2, 0, MaterialPalette.CHARACTER_BONE, "R_forearm_bone", 0xFFFF6600),
        new Segment(5, 1, 0, 6, 1, 0, MaterialPalette.CHARACTER_FLESH, "R_forearm_flesh", 0xFFCC3300),

        // Left upper leg
        new Segment(0, 0, -1, 2, 4, 0, MaterialPalette.CHARACTER_BONE, "L_thigh_bone", 0xFFFF6600),
        new Segment(1, 1, 0, 1, 3, 0, MaterialPalette.CHARACTER_FLESH, "L_thigh_flesh", 0xFFCC3300),

        // Right upper leg
        new Segment(2, 0, -1, 4, 4, 0, MaterialPalette.CHARACTER_BONE, "R_thigh_bone", 0xFFFF6600),
        new Segment(3, 1, 0, 3, 3, 0, MaterialPalette.CHARACTER_FLESH, "R_thigh_flesh", 0xFFCC3300),

        // Left lower leg
        new Segment(0, -3, -1, 1, 0, 0, MaterialPalette.CHARACTER_BONE, "L_calf_bone", 0xFFFF6600),
        new Segment(0, -2, 0, 1, -1, 0, MaterialPalette.CHARACTER_FLESH, "L_calf_flesh", 0xFFCC3300),

        // Right lower leg
        new Segment(3, -3, -1, 4, 0, 0, MaterialPalette.CHARACTER_BONE, "R_calf_bone", 0xFFFF6600),
        new Segment(3, -2, 0, 4, -1, 0, MaterialPalette.CHARACTER_FLESH, "R_calf_flesh", 0xFFCC3300),
    };

    /**
     * Paints the hitbox wireframe overlay onto a VoxelGrid.
     * Uses CUSTOM material with wireframe colors.
     * @param g Target grid (should be empty or cosmetic layer)
     * @param fx Feet X offset
     * @param fy Feet Y offset
     * @param fz Feet Z offset
     */
    public static void paintHitboxWireframe(VoxelGrid g, int fx, int fy, int fz) {
        g.assertCubicUnitInvariant();
        for (Segment s : DEFAULT_SEGMENTS) {
            paintWireframeBox(g, fx + s.x0, fy + s.y0, fz + s.z0,
                             fx + s.x1, fy + s.y1, fz + s.z1, s.color);
        }
    }

    /**
     * Paints filled hitbox segments with material IDs (for reference).
     * @param g Target grid
     * @param fx Feet X offset
     * @param fy Feet Y offset
     * @param fz Feet Z offset
     */
    public static void paintHitboxFilled(VoxelGrid g, int fx, int fy, int fz) {
        g.assertCubicUnitInvariant();
        for (Segment s : DEFAULT_SEGMENTS) {
            for (int z = s.z0; z <= s.z1; z++)
                for (int y = s.y0; y <= s.y1; y++)
                    for (int x = s.x0; x <= s.x1; x++)
                        g.set(fx + x, fy + y, fz + z, s.materialId,
                              MaterialPalette.defaultRgb(s.materialId));
        }
    }

    /**
     * Paints a wireframe box outline (edges only).
     */
    private static void paintWireframeBox(VoxelGrid g, int x0, int y0, int z0,
                                          int x1, int y1, int z1, int color) {
        // 12 edges of a box
        // Bottom face (y=y0)
        drawLine(g, x0, y0, z0, x1, y0, z0, color);
        drawLine(g, x1, y0, z0, x1, y0, z1, color);
        drawLine(g, x1, y0, z1, x0, y0, z1, color);
        drawLine(g, x0, y0, z1, x0, y0, z0, color);
        // Top face (y=y1)
        drawLine(g, x0, y1, z0, x1, y1, z0, color);
        drawLine(g, x1, y1, z0, x1, y1, z1, color);
        drawLine(g, x1, y1, z1, x0, y1, z1, color);
        drawLine(g, x0, y1, z1, x0, y1, z0, color);
        // Vertical edges
        drawLine(g, x0, y0, z0, x0, y1, z0, color);
        drawLine(g, x1, y0, z0, x1, y1, z0, color);
        drawLine(g, x1, y0, z1, x1, y1, z1, color);
        drawLine(g, x0, y0, z1, x0, y1, z1, color);
    }

    /**
     * 3D Bresenham line drawing for wireframe edges.
     */
    private static void drawLine(VoxelGrid g, int x0, int y0, int z0,
                                 int x1, int y1, int z1, int color) {
        int dx = Math.abs(x1 - x0), dy = Math.abs(y1 - y0), dz = Math.abs(z1 - z0);
        int sx = x0 < x1 ? 1 : -1;
        int sy = y0 < y1 ? 1 : -1;
        int sz = z0 < z1 ? 1 : -1;
        int dm = Math.max(dx, Math.max(dy, dz));
        if (dm == 0) {
            g.set(x0, y0, z0, MaterialPalette.CUSTOM, color);
            return;
        }
        for (int i = 0; i <= dm; i++) {
            int x = x0 + (dx == 0 ? 0 : (i * (x1 - x0)) / dm);
            int y = y0 + (dy == 0 ? 0 : (i * (y1 - y0)) / dm);
            int z = z0 + (dz == 0 ? 0 : (i * (z1 - z0)) / dm);
            g.set(x, y, z, MaterialPalette.CUSTOM, color);
        }
    }

    /**
     * Returns segment info for UI display.
     */
    public static List<Segment> getSegments() {
        List<Segment> list = new ArrayList<>();
        for (Segment s : DEFAULT_SEGMENTS) list.add(s);
        return list;
    }
}