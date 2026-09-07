package voxel.painter.grid;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/** Engine-synced material ids + optional RGB for sky/dropper paint. */
public final class MaterialPalette {
    public static final int AIR = 0;
    public static final int WOOD = 1;
    public static final int CONCRETE = 2;
    public static final int DIRT = 3;
    public static final int BUSH_LEAVES = 4;
    public static final int BUSH_BRANCH = 5;
    public static final int SHEET_METAL = 6;
    public static final int GIRDER = 7;
    public static final int WATER = 8;
    public static final int PLEXIGLASS = 9;
    public static final int CARBON_FIBER = 10;
    public static final int TREATED_WOOD = 11;
    public static final int CUSTOM = 12; // free RGB (sky)

    public static final class Entry {
        public final int id;
        public final String name;
        public final int rgb; // 0xRRGGBB

        public Entry(int id, String name, int rgb) {
            this.id = id;
            this.name = name;
            this.rgb = rgb & 0xFFFFFF;
        }
    }

    private static final List<Entry> ENTRIES;
    private static final Map<String, Integer> BY_NAME;

    static {
        List<Entry> list = new ArrayList<>();
        list.add(new Entry(AIR, "air", 0x000000));
        list.add(new Entry(WOOD, "wood", 0x57381F));
        list.add(new Entry(CONCRETE, "concrete", 0x66666B));
        list.add(new Entry(DIRT, "dirt", 0x47331F));
        list.add(new Entry(BUSH_LEAVES, "bush_leaves", 0x2E8B3A));
        list.add(new Entry(BUSH_BRANCH, "bush_branch", 0x523016));
        list.add(new Entry(SHEET_METAL, "sheet_metal", 0x7A8085));
        list.add(new Entry(GIRDER, "girder", 0x471A14));
        list.add(new Entry(WATER, "water", 0x1F4770));
        list.add(new Entry(PLEXIGLASS, "plexiglass", 0xA8D4E6));
        list.add(new Entry(CARBON_FIBER, "carbon_fiber", 0x1A1A1E));
        list.add(new Entry(TREATED_WOOD, "treated_wood", 0x6B4423));
        list.add(new Entry(CUSTOM, "custom", 0xFFFFFF));
        ENTRIES = Collections.unmodifiableList(list);
        Map<String, Integer> map = new LinkedHashMap<>();
        for (Entry e : ENTRIES) map.put(e.name, e.id);
        BY_NAME = Collections.unmodifiableMap(map);
    }

    private MaterialPalette() {}

    public static List<Entry> entries() {
        return ENTRIES;
    }

    public static int idFromName(String name) {
        if (name == null) return AIR;
        Integer id = BY_NAME.get(name.toLowerCase(Locale.ROOT));
        return id == null ? AIR : id;
    }

    public static String nameFromId(int id) {
        for (Entry e : ENTRIES) if (e.id == id) return e.name;
        return "air";
    }

    public static int defaultRgb(int id) {
        for (Entry e : ENTRIES) if (e.id == id) return e.rgb;
        return 0xFF00FF;
    }
}
