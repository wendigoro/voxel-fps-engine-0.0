package voxel.painter.ui;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.PaintTools;
import voxel.painter.grid.SkyAndCharacter;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxelGrid;
import voxel.painter.grid.WeaponParts;

/** Shared mutable session state over real voxel.painter.grid types. */
public final class PainterModel {
    public interface Listener {
        void documentChanged();
        void toolsChanged();
        void sliceChanged();
    }

    public enum SliceAxis { X, Y, Z }

    public enum UiTool { BRUSH, FILL, STRETCH, DROPPER, LINE }

    private final List<Listener> listeners = new CopyOnWriteArrayList<>();
    private final PaintTools tools = new PaintTools();

    private VoxDocument document = new VoxDocument(VoxDocument.Mode.MODEL, 32, 24, 32);
    private Path filePath;
    private boolean dirty;

    private UiTool uiTool = UiTool.BRUSH;
    private int gradA = 0x1A3A6E;
    private int gradB = 0xE8C060;
    private float gradientT = 0.5f;
    private boolean useGradientRgb;

    private SliceAxis sliceAxis = SliceAxis.Y;
    private int sliceIndex = 0;

    private Integer stretchAx, stretchAy, stretchAz;
    private Integer lineAx, lineAy, lineAz;
    private int stretchFactor = 2;

    public void addListener(Listener l) { listeners.add(l); }

    private void fireDoc() { for (Listener l : listeners) l.documentChanged(); }
    private void fireTools() { for (Listener l : listeners) l.toolsChanged(); }
    private void fireSlice() { for (Listener l : listeners) l.sliceChanged(); }

    public PaintTools tools() { return tools; }
    public VoxDocument document() { return document; }
    public VoxelGrid grid() { return document.grid; }
    public Path filePath() { return filePath; }
    public boolean isDirty() { return dirty; }

    public void markDirty() { dirty = true; fireDoc(); }

    public void setDocument(VoxDocument doc, Path path, boolean markClean) {
        this.document = doc;
        this.filePath = path;
        this.dirty = !markClean;
        doc.grid.assertCubicUnitInvariant();
        clampSlice();
        clearAnchors();
        fireDoc();
        fireSlice();
    }

public void newDocument(VoxDocument.Mode mode) {
        VoxDocument doc = switch (mode) {
            case SKY -> new VoxDocument(VoxDocument.Mode.SKY, 28, 1, 14);
            case CHARACTER -> new VoxDocument(VoxDocument.Mode.CHARACTER, 24, 16, 12);
            case WEAPON -> new VoxDocument(VoxDocument.Mode.WEAPON, 24, 12, 12);
            default -> new VoxDocument(VoxDocument.Mode.MODEL, 32, 24, 32);
        };
        if (mode == VoxDocument.Mode.SKY) {
            SkyAndCharacter.paintSkyTiles(doc.grid, doc.segU, doc.segV,
                    doc.moonDirX, doc.moonDirY, doc.moonDirZ, doc.moonIntensity);
        } else if (mode == VoxDocument.Mode.CHARACTER) {
            doc.feetX = 8; doc.feetY = 0; doc.feetZ = 4;
            SkyAndCharacter.paintCharacter(doc.grid, doc.feetX, doc.feetY, doc.feetZ);
        } else if (mode == VoxDocument.Mode.WEAPON) {
            doc.caliber = "medium";
            doc.ammoId = "medium_fmj";
            WeaponParts.paintStarterRifle(doc.grid);
            tools.activePart = WeaponParts.BARREL;
        } else {
            seedDemo(doc);
        }
        setDocument(doc, null, true);
    }

    public void setActivePart(int partId) {
        tools.activePart = partId;
        fireTools();
    }

    public int activePart() { return tools.activePart; }

    public void setCaliber(String caliber) {
        if (caliber != null) document.caliber = caliber;
        fireDoc();
    }

    public void bakeStarterWeapon() {
        if (document.mode != VoxDocument.Mode.WEAPON) {
            newDocument(VoxDocument.Mode.WEAPON);
            return;
        }
        WeaponParts.paintStarterRifle(document.grid);
        markDirty();
    }

    public WeaponParts.Stats weaponStats() {
        return WeaponParts.compose(document.grid, document.caliber);
    }

    public static void seedDemo(VoxDocument doc) {
        VoxelGrid g = doc.grid;
        int cx = g.sizeX() / 2, cy = g.sizeY() / 2, cz = g.sizeZ() / 2;
        for (int z = cz - 1; z <= cz + 1; z++)
            for (int y = cy - 1; y <= cy + 1; y++)
                for (int x = cx - 1; x <= cx + 1; x++)
                    g.setMat(x, y, z, MaterialPalette.WOOD);
        g.setMat(cx, cy + 2, cz, MaterialPalette.BUSH_LEAVES);
    }

    public UiTool uiTool() { return uiTool; }
    public void setUiTool(UiTool t) {
        this.uiTool = t == null ? UiTool.BRUSH : t;
        clearAnchors();
        fireTools();
    }

    public void setBrushShape(PaintTools.BrushShape shape) {
        tools.shape = shape == null ? PaintTools.BrushShape.CUBE : shape;
        fireTools();
    }

    public void setBrushSize(int size) {
        int s = Math.max(1, size);
        if ((s & 1) == 0) s -= 1;
        tools.brushSize = Math.max(1, s);
        fireTools();
    }

    public void setPrimaryMaterial(int id) {
        tools.matA = id;
        tools.rgbA = MaterialPalette.defaultRgb(id);
        tools.useB = false;
        fireTools();
    }

    public void setAltMaterial(int id) {
        tools.matB = id;
        tools.rgbB = MaterialPalette.defaultRgb(id);
        fireTools();
    }

    public void toggleAlt() { tools.toggleAlternate(); fireTools(); }

    public int gradA() { return gradA; }
    public int gradB() { return gradB; }
    public float gradientT() { return gradientT; }
    public boolean useGradientRgb() { return useGradientRgb; }

    public void setGradA(int rgb) { gradA = rgb & 0xFFFFFF; fireTools(); }
    public void setGradB(int rgb) { gradB = rgb & 0xFFFFFF; fireTools(); }

    public void setGradientT(float t) {
        gradientT = Math.max(0f, Math.min(1f, t));
        if (useGradientRgb) tools.sampleFGradient(gradA, gradB, gradientT);
        fireTools();
    }

    public void setUseGradientRgb(boolean v) {
        useGradientRgb = v;
        if (v) tools.sampleFGradient(gradA, gradB, gradientT);
        fireTools();
    }

    public SliceAxis sliceAxis() { return sliceAxis; }
    public int sliceIndex() { return sliceIndex; }

    public void setSliceAxis(SliceAxis axis) {
        this.sliceAxis = axis == null ? SliceAxis.Y : axis;
        clampSlice();
        fireSlice();
    }

    public void setSliceIndex(int index) {
        this.sliceIndex = index;
        clampSlice();
        fireSlice();
    }

    public int stretchFactor() { return stretchFactor; }
    public void setStretchFactor(int f) { stretchFactor = Math.max(1, f); fireTools(); }

    public int maxSlice() {
        return switch (sliceAxis) {
            case X -> Math.max(0, grid().sizeX() - 1);
            case Y -> Math.max(0, grid().sizeY() - 1);
            case Z -> Math.max(0, grid().sizeZ() - 1);
        };
    }

    private void clampSlice() {
        sliceIndex = Math.max(0, Math.min(sliceIndex, maxSlice()));
    }

    private void clearAnchors() {
        stretchAx = stretchAy = stretchAz = null;
        lineAx = lineAy = lineAz = null;
    }

    public int[] sliceToWorld(int u, int v) {
        return switch (sliceAxis) {
            case X -> new int[] {sliceIndex, u, v};
            case Y -> new int[] {u, sliceIndex, v};
            case Z -> new int[] {u, v, sliceIndex};
        };
    }

    public int planeWidth() {
        return switch (sliceAxis) {
            case X -> grid().sizeY();
            case Y -> grid().sizeX();
            case Z -> grid().sizeX();
        };
    }

    public int planeHeight() {
        return switch (sliceAxis) {
            case X -> grid().sizeZ();
            case Y -> grid().sizeZ();
            case Z -> grid().sizeY();
        };
    }

    public void applyToolAt(int x, int y, int z, boolean erase) {
        VoxelGrid g = grid();
        if (!g.inBounds(x, y, z)) return;
        g.assertCubicUnitInvariant();

        if (uiTool == UiTool.DROPPER) {
            tools.dropper(g, x, y, z);
            fireTools();
            return;
        }

        if (erase) {
            int saveMat = tools.activeMat();
            int saveRgb = tools.activeRgb();
            boolean useB = tools.useB;
            if (useB) { tools.matB = MaterialPalette.AIR; tools.rgbB = 0; }
            else { tools.matA = MaterialPalette.AIR; tools.rgbA = 0; }
            try {
                if (uiTool == UiTool.FILL) tools.floodFill(g, x, y, z);
                else tools.paintAt(g, x, y, z);
            } finally {
                if (useB) { tools.matB = saveMat; tools.rgbB = saveRgb; }
                else { tools.matA = saveMat; tools.rgbA = saveRgb; }
            }
            markDirty();
            return;
        }

        if (useGradientRgb) tools.sampleFGradient(gradA, gradB, gradientT);

        switch (uiTool) {
            case FILL -> { tools.floodFill(g, x, y, z); markDirty(); }
            case LINE -> {
                if (lineAx == null) {
                    lineAx = x; lineAy = y; lineAz = z; fireTools();
                } else {
                    tools.paintLine(g, lineAx, lineAy, lineAz, x, y, z);
                    clearAnchors();
                    markDirty();
                }
            }
            case STRETCH -> {
                if (stretchAx == null) {
                    stretchAx = x; stretchAy = y; stretchAz = z; fireTools();
                } else {
                    PaintTools.stretch(g, stretchAx, stretchAy, stretchAz, x, y, z,
                            stretchFactor, stretchFactor, stretchFactor);
                    clearAnchors();
                    markDirty();
                }
            }
            default -> { tools.paintAt(g, x, y, z); markDirty(); }
        }
    }

    public String statusLine() {
        List<String> bits = new ArrayList<>();
        bits.add(document.modeName());
        bits.add(uiTool.name().toLowerCase());
        bits.add(tools.shape.name().toLowerCase());
        bits.add("size=" + tools.brushSize);
bits.add("mat=" + MaterialPalette.nameFromId(tools.activeMat()) + (tools.useB ? "(B)" : "(A)"));
        if (document.mode == VoxDocument.Mode.WEAPON) {
            bits.add("part=" + WeaponParts.name(tools.activePart));
            bits.add("cal=" + document.caliber);
            WeaponParts.Stats st = WeaponParts.compose(document.grid, document.caliber);
            bits.add(String.format("dmg=%.1f imp=%.1f rec=%.1f hnd=%.1f w=%.1f", st.damage, st.impact, st.recoil, st.handling, st.weight));
        }
        bits.add("slice=" + sliceAxis + ":" + sliceIndex);
        bits.add("unit=" + grid().unitSizeX() + "=" + grid().unitSizeY() + "=" + grid().unitSizeZ());
        bits.add("VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE);
        bits.add("solids=" + grid().solidCount());
        if (filePath != null) bits.add(filePath.getFileName().toString());
        if (dirty) bits.add("*");
        if (lineAx != null) bits.add("line-anchor");
        if (stretchAx != null) bits.add("stretch-anchor");
        return String.join(" | ", bits);
    }
}
