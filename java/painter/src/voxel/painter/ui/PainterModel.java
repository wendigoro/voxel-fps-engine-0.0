package voxel.painter.ui;

import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import voxel.painter.grid.Items;
import voxel.painter.grid.MapEntities;
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

    public enum UiTool { BRUSH, FILL, STRETCH, DROPPER, LINE, PLACE_EVENT, PLACE_NPC, SELECT_ENTITY }

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
            // Small by default: an item grid IS its packing footprint, so a big
            // default would be a huge item the player could never pick up.
              case ITEM -> new VoxDocument(VoxDocument.Mode.ITEM, 4, 3, 2);
              // A map is the one document that wants to be roomy, so it defaults
              // wider than the models. Still the cubic unit grid throughout.
              case MAP -> new VoxDocument(VoxDocument.Mode.MAP, 64, 32, 64);
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
            doc.fireMode = "semi";
            WeaponParts.paintStarterRifle(doc.grid);
            tools.activePart = WeaponParts.BARREL;
        } else if (mode == VoxDocument.Mode.ITEM) {
            doc.itemId = "item_pouch_medium";
            doc.itemName = "Medium Ammo Pouch";
            doc.itemClass = Items.CLASS_AMMO_POUCH;
            doc.armorZone = "";
            doc.packSX = 0; doc.packSY = 0; doc.packSZ = 0;
            doc.caliber = "medium";
            doc.ammoId = "medium_fmj";
              Items.paintStarterPouch(doc.grid);
          } else if (mode == VoxDocument.Mode.MAP) {
              seedMap(doc);
          } else {
              seedDemo(doc);
          }
          setDocument(doc, null, true);
      }

    /** Lay down a starter arena so a new map document is not an empty box. */
    public static void seedMap(VoxDocument doc) {
        VoxelGrid g = doc.grid;
        int cx = g.sizeX() / 2, cz = g.sizeZ() / 2;
        for (int z = 0; z < g.sizeZ(); z++)
            for (int x = 0; x < g.sizeX(); x++)
                g.setMat(x, 0, z, MaterialPalette.CONCRETE);
        for (int y = 1; y < 8; y++) {
            for (int x = 0; x < g.sizeX(); x++) {
                g.setMat(x, y, 0, MaterialPalette.SHEET_METAL);
                g.setMat(x, y, g.sizeZ() - 1, MaterialPalette.SHEET_METAL);
            }
            for (int z = 0; z < g.sizeZ(); z++) {
                g.setMat(0, y, z, MaterialPalette.SHEET_METAL);
                g.setMat(g.sizeX() - 1, y, z, MaterialPalette.SHEET_METAL);
            }
        }
        for (int i = 0; i < 3; i++) {
            int bx = cx + (i - 1) * 10;
            int bz = cz + (i - 1) * 8;
            for (int z = bz; z < bz + 4; z++)
                for (int y = 1; y < 5; y++)
                    for (int x = bx; x < bx + 4; x++)
                        g.setMat(x, y, z, MaterialPalette.WOOD);
        }
    }

    // ---- MAP mode: entity placement ---------------------------------------
    // Placement tools write integers onto the same cubic unit grid the voxels
    // occupy. An entity never carries a world-space position, so a map can be
    // rescaled by changing VOXEL_SIZE alone.

    private String pendingEventScript = "trigger_door_open.ps1";
    private String pendingEventTrigger = "on_enter";
    private String pendingNpcType = "guard";
    private String pendingNpcAiProfile = "patrol";
    private String selectedEntity = "";

    public String pendingEventScript() { return pendingEventScript; }
    public void setPendingEventScript(String s) { pendingEventScript = s; }
    public String pendingEventTrigger() { return pendingEventTrigger; }
    public void setPendingEventTrigger(String s) { pendingEventTrigger = s; }
    public String pendingNpcType() { return pendingNpcType; }
    public void setPendingNpcType(String s) { pendingNpcType = s; }
    public String pendingNpcAiProfile() { return pendingNpcAiProfile; }
    public void setPendingNpcAiProfile(String s) { pendingNpcAiProfile = s; }
    public String selectedEntity() { return selectedEntity; }

    public void placeScriptedEvent(int x, int y, int z) {
        if (document.mode != VoxDocument.Mode.MAP) return;
        if (!grid().inBounds(x, y, z)) return;
        MapEntities.ScriptedEvent e = new MapEntities.ScriptedEvent(
                x, y, z,
                document.mapData.nextEventId(),
                "Event " + (document.mapData.events.size() + 1),
                pendingEventScript);
        e.triggerType = pendingEventTrigger;
        document.mapData.addEvent(e);
        selectedEntity = e.id;
        markDirty();
    }

    public void placeNpc(int x, int y, int z) {
        if (document.mode != VoxDocument.Mode.MAP) return;
        if (!grid().inBounds(x, y, z)) return;
        MapEntities.Npc n = new MapEntities.Npc(
                x, y, z,
                document.mapData.nextNpcId(),
                "NPC " + (document.mapData.npcs.size() + 1),
                pendingNpcType);
        n.aiProfile = pendingNpcAiProfile;
        document.mapData.addNpc(n);
        selectedEntity = n.id;
        markDirty();
    }

    /** Select whatever map entity sits on this cell, for the status readout. */
    public void selectEntityAt(int x, int y, int z) {
        if (document.mode != VoxDocument.Mode.MAP) return;
        MapEntities.ScriptedEvent e = findEventAt(x, y, z);
        if (e != null) {
            selectedEntity = e.id;
        } else {
            MapEntities.Npc n = findNpcAt(x, y, z);
            selectedEntity = n != null ? n.id : "";
        }
    }

    public void removeEntityAt(int x, int y, int z) {
        if (document.mode != VoxDocument.Mode.MAP) return;
        MapEntities.MapData md = document.mapData;
        md.events.removeIf(e -> e.x == x && e.y == y && e.z == z);
        md.npcs.removeIf(n -> n.x == x && n.y == y && n.z == z);
        selectedEntity = "";
        markDirty();
    }

    public MapEntities.ScriptedEvent findEventAt(int x, int y, int z) {
        for (MapEntities.ScriptedEvent e : document.mapData.events) {
            if (e.x == x && e.y == y && e.z == z) return e;
        }
        return null;
    }

    public MapEntities.Npc findNpcAt(int x, int y, int z) {
        for (MapEntities.Npc n : document.mapData.npcs) {
            if (n.x == x && n.y == y && n.z == z) return n;
        }
        return null;
    }

    public MapEntities.MapData mapData() {
        return document.mapData;
    }

    /** True for the tools that address map entities rather than voxels. */
    public static boolean isEntityTool(UiTool t) {
        return t == UiTool.PLACE_EVENT || t == UiTool.PLACE_NPC || t == UiTool.SELECT_ENTITY;
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

    public void setAmmoId(String ammoId) {
        if (ammoId != null && !ammoId.isBlank()) document.ammoId = ammoId;
        fireDoc();
    }

    public void setFireMode(String fireMode) {
        if (fireMode == null) return;
        String fm = fireMode.toLowerCase();
        if (fm.equals("semi") || fm.equals("auto") || fm.equals("bolt")) {
            document.fireMode = fm;
            fireDoc();
        }
    }

    public void bakeStarterWeapon() {
        if (document.mode != VoxDocument.Mode.WEAPON) {
            newDocument(VoxDocument.Mode.WEAPON);
            return;
        }
        WeaponParts.paintStarterRifle(document.grid);
        document.caliber = document.caliber == null ? "medium" : document.caliber;
        document.ammoId = document.ammoId == null ? "medium_fmj" : document.ammoId;
        document.fireMode = document.fireMode == null ? "semi" : document.fireMode;
        tools.activePart = WeaponParts.BARREL;
        markDirty();
        fireTools();
    }

    public void setItemId(String id) {
        if (id == null || id.isBlank()) return;
        document.itemId = id;
        fireDoc();
    }

    public void setItemName(String name) {
        if (name == null || name.isBlank()) return;
        document.itemName = name;
        fireDoc();
    }

    public void setItemClass(String cls) {
        if (cls == null) return;
        for (String c : Items.CLASSES)
            if (c.equals(cls)) { document.itemClass = cls; break; }
        fireDoc();
    }

    public void setArmorZone(String zone) {
        if (zone == null) return;
        for (String z : Items.ARMOR_ZONES)
            if (z.equals(zone)) { document.armorZone = zone; fireDoc(); return; }
        // Empty selection clears the zone, which is how a non-armor item is authored.
        document.armorZone = "";
        fireDoc();
    }

    public void setPackSize(int sx, int sy, int sz) {
        document.packSX = Math.max(0, sx);
        document.packSY = Math.max(0, sy);
        document.packSZ = Math.max(0, sz);
        fireDoc();
    }

    /** Re-bake the starter pouch template into the current item document. */
    public void bakeStarterItem() {
        if (document.mode != VoxDocument.Mode.ITEM) {
            newDocument(VoxDocument.Mode.ITEM);
            return;
        }
        Items.paintStarterPouch(document.grid);
        markDirty();
    }

    public WeaponParts.Stats weaponStats() {
        WeaponParts.Stats s = WeaponParts.compose(document.grid, document.caliber);
        // Prefer explicit document fire mode when user set one in the UI.
        if (document.fireMode != null && !document.fireMode.isBlank()) {
            s.fireMode = document.fireMode;
        } else {
            document.fireMode = s.fireMode;
        }
        return s;
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
            // In MAP mode the entity tools erase entities, not voxels: a map
            // author should not have to switch tools to clear an NPC.
            if (document.mode == VoxDocument.Mode.MAP && isEntityTool(uiTool)) {
                removeEntityAt(x, y, z);
                return;
            }
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
              case PLACE_EVENT -> placeScriptedEvent(x, y, z);
              case PLACE_NPC -> placeNpc(x, y, z);
              case SELECT_ENTITY -> { selectEntityAt(x, y, z); fireTools(); }
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
            bits.add("ammo=" + document.ammoId);
            bits.add("fire=" + document.fireMode);
            WeaponParts.Stats st = weaponStats();
            bits.add(String.format(
                    "dmg=%.1f imp=%.1f rec=%.1f hnd=%.1f w=%.1f opt=%.1f",
                    st.damage, st.impact, st.recoil, st.handling, st.weight, st.optic));
        }
        if (document.mode == VoxDocument.Mode.ITEM) {
            // The shape is the painted grid, so show the storage cost the engine
            // will actually charge for picking this up.
            bits.add("item=" + document.itemId);
            bits.add("class=" + document.itemClass);
            if (Items.needsArmorZone(document.itemClass) && !document.armorZone.isEmpty())
                bits.add("zone=" + document.armorZone);
            if (Items.needsPackSize(document.itemClass) && document.packSX > 0)
                bits.add("pack=" + document.packSX + "x" + document.packSY + "x" + document.packSZ);
              bits.add("cells=" + Items.solidCells(grid()));
          }
          if (document.mode == VoxDocument.Mode.MAP) {
              bits.add("events=" + document.mapData.events.size());
              bits.add("npcs=" + document.mapData.npcs.size());
              bits.add("routes=" + document.mapData.patrolRoutes.size());
              if (!selectedEntity.isEmpty()) bits.add("sel=" + selectedEntity);
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
