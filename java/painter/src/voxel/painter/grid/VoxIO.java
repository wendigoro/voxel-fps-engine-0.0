package voxel.painter.grid;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Minimal JSON IO without external deps (hand-rolled subset). */
public final class VoxIO {
    private VoxIO() {}

    /** Current document format. Files without the key default to 1.
     *  v1: a "voxels" list, one object per cell (still read).
     *  v2: run-length layers shared with the engine's maps (data/voxfmt/schema.md,
     *      "Format v2"): cells_rle (materials), appearance (paint that differs
     *      from the material's own colour), parts_rle (weapon parts), anchors. */
    public static final int FORMAT_VERSION = 2;

    public static void save(VoxDocument doc, Path path) throws IOException {
        doc.grid.assertCubicUnitInvariant();
        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
        sb.append("  \"format_version\": ").append(FORMAT_VERSION).append(",\n");
        sb.append("  \"unit\": ").append(doc.unit).append(",\n");
        sb.append("  \"voxel_size\": ").append(doc.voxelSize).append(",\n");
        sb.append("  \"mode\": \"").append(doc.modeName()).append("\",\n");
        sb.append("  \"dims\": [").append(doc.grid.sizeX()).append(", ")
                .append(doc.grid.sizeY()).append(", ").append(doc.grid.sizeZ()).append("],\n");
        if (doc.mode == VoxDocument.Mode.SKY) {
            sb.append("  \"seg_u\": ").append(doc.segU).append(",\n");
            sb.append("  \"seg_v\": ").append(doc.segV).append(",\n");
            sb.append("  \"moon_dir\": [").append(doc.moonDirX).append(", ")
                    .append(doc.moonDirY).append(", ").append(doc.moonDirZ).append("],\n");
            sb.append("  \"moon_intensity\": ").append(doc.moonIntensity).append(",\n");
        }
        // Anchors: named attachment cells. Every model has a pivot (bottom
        // centre of its grid); characters add their feet.
        sb.append("  \"anchors\": {\"pivot\": [").append(doc.grid.sizeX() / 2).append(", 0, ")
                .append(doc.grid.sizeZ() / 2).append("]");
        if (doc.mode == VoxDocument.Mode.CHARACTER) {
            sb.append(", \"feet\": [").append(doc.feetX).append(", ")
                    .append(doc.feetY).append(", ").append(doc.feetZ).append("]");
        }
        sb.append("},\n");
        if (doc.mode == VoxDocument.Mode.WEAPON) {
            sb.append("  \"caliber\": \"").append(doc.caliber).append("\",\n");
            sb.append("  \"ammo_id\": \"").append(doc.ammoId).append("\",\n");
        }
        if (doc.mode == VoxDocument.Mode.ITEM) {
            sb.append("  \"item_id\": \"").append(doc.itemId).append("\",\n");
            sb.append("  \"item_name\": \"").append(doc.itemName).append("\",\n");
            sb.append("  \"item_class\": \"").append(doc.itemClass).append("\",\n");
            sb.append("  \"armor_zone\": \"").append(doc.armorZone).append("\",\n");
            sb.append("  \"pack_size\": [").append(doc.packSX).append(", ")
                    .append(doc.packSY).append(", ").append(doc.packSZ).append("],\n");
        }
        if (doc.mode == VoxDocument.Mode.MAP) {
            int[] counters = doc.mapData.counters();
            sb.append("  \"id_counters\": {\"evt\": ").append(counters[0])
                    .append(", \"npc\": ").append(counters[1])
                    .append(", \"route\": ").append(counters[2]).append("},\n");
            saveMapEntities(sb, doc.mapData);
        }
        VoxelGrid g = doc.grid;
        // Three run-length layers along +X, one (y, z) row at a time.
        // Materials: a run of one material id.
        List<String> matNames = new ArrayList<>();
        StringBuilder matRuns = new StringBuilder();
        // Paint: a run of one colour, only where it differs from the material's own.
        List<Integer> paintColors = new ArrayList<>();
        StringBuilder paintRuns = new StringBuilder();
        // Parts: a run of one weapon part id.
        List<String> partNames = new ArrayList<>();
        StringBuilder partRuns = new StringBuilder();
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++) {
                for (int x = 0; x < g.sizeX(); ) {
                    int m = g.getMat(x, y, z);
                    int len = 1;
                    while (x + len < g.sizeX() && g.getMat(x + len, y, z) == m) len++;
                    if (m != MaterialPalette.AIR) {
                        String n = MaterialPalette.nameFromId(m);
                        int idx = matNames.indexOf(n);
                        if (idx < 0) { matNames.add(n); idx = matNames.size() - 1; }
                        appendRun(matRuns, y, z, x, len, idx);
                    }
                    x += len;
                }
                for (int x = 0; x < g.sizeX(); ) {
                    int c = paintOf(g, x, y, z);
                    int len = 1;
                    while (x + len < g.sizeX() && paintOf(g, x + len, y, z) == c) len++;
                    if (c >= 0) {
                        int idx = paintColors.indexOf(c);
                        if (idx < 0 && paintColors.size() < 255) { paintColors.add(c); idx = paintColors.size() - 1; }
                        if (idx < 0) idx = nearestColor(paintColors, c);
                        appendRun(paintRuns, y, z, x, len, idx + 1);
                    }
                    x += len;
                }
                for (int x = 0; x < g.sizeX(); ) {
                    int p = g.getMat(x, y, z) == MaterialPalette.AIR ? 0 : g.getPart(x, y, z);
                    int len = 1;
                    while (x + len < g.sizeX() && (g.getMat(x + len, y, z) == MaterialPalette.AIR ? 0
                            : g.getPart(x + len, y, z)) == p) len++;
                    if (p > 0) {
                        String n = WeaponParts.name(p);
                        int idx = partNames.indexOf(n);
                        if (idx < 0) { partNames.add(n); idx = partNames.size() - 1; }
                        appendRun(partRuns, y, z, x, len, idx);
                    }
                    x += len;
                }
            }
        if (!paintColors.isEmpty()) {
            sb.append("  \"appearance\": {\"palette\": [");
            for (int i = 0; i < paintColors.size(); i++) {
                int c = paintColors.get(i);
                if (i > 0) sb.append(", ");
                sb.append('[').append((c >> 16) & 255).append(", ").append((c >> 8) & 255).append(", ")
                        .append(c & 255).append(']');
            }
            sb.append("], \"runs\": [").append(paintRuns).append("]},\n");
        }
        if (!partNames.isEmpty()) {
            sb.append("  \"parts_rle\": {\"palette\": ").append(quoted(partNames))
                    .append(", \"runs\": [").append(partRuns).append("]},\n");
        }
        sb.append("  \"cells_rle\": {\"palette\": ").append(quoted(matNames))
                .append(", \"runs\": [").append(matRuns).append("]}\n}\n");
        Files.createDirectories(path.getParent() == null ? Path.of(".") : path.getParent());
        Files.writeString(path, sb.toString(), StandardCharsets.UTF_8);
    }

    public static VoxDocument load(Path path) throws IOException {
        String text = Files.readString(path, StandardCharsets.UTF_8);
        int formatVersion = findInt(text, "format_version", 1);
        if (formatVersion > FORMAT_VERSION) {
            throw new IOException("unsupported voxfmt format_version " + formatVersion
                    + " (this writer understands up to " + FORMAT_VERSION + ")");
        }
        String mode = findString(text, "mode", "model");
        int[] dims = findIntArray(text, "dims", new int[]{16, 16, 16});
        VoxDocument doc = new VoxDocument(VoxDocument.parseMode(mode), dims[0], dims[1], dims[2]);
        doc.segU = findInt(text, "seg_u", 28);
        doc.segV = findInt(text, "seg_v", 14);
        float[] moon = findFloatArray(text, "moon_dir", new float[]{0.32f, 0.82f, -0.48f});
        doc.moonDirX = moon[0]; doc.moonDirY = moon[1]; doc.moonDirZ = moon[2];
        doc.moonIntensity = findFloat(text, "moon_intensity", 0.95f);
        int[] feet = findIntArray(text, "feet", new int[]{0, 0, 0}); // v1 key and v2 anchor share the name
doc.feetX = feet[0]; doc.feetY = feet[1]; doc.feetZ = feet[2];
        doc.caliber = findString(text, "caliber", doc.caliber);
        doc.ammoId = findString(text, "ammo_id", doc.ammoId);
        doc.itemId = findString(text, "item_id", doc.itemId);
        doc.itemName = findString(text, "item_name", doc.itemName);
        doc.itemClass = findString(text, "item_class", doc.itemClass);
        doc.armorZone = findString(text, "armor_zone", doc.armorZone);
        int[] pack = findIntArray(text, "pack_size", new int[]{0, 0, 0});
        doc.packSX = pack[0]; doc.packSY = pack[1]; doc.packSZ = pack[2];
        if (doc.mode == VoxDocument.Mode.MAP) {
            loadMapEntities(text, doc.mapData);
            int evt = findInt(text, "evt", 1);
            int npc = findInt(text, "npc", 1);
            int route = findInt(text, "route", 1);
            doc.mapData.restoreCounters(evt, npc, route);
        }

        if (text.contains("\"cells_rle\"")) {
            loadRunLayers(text, doc);
            doc.grid.assertCubicUnitInvariant();
            return doc;
        }

        // v1: one object per cell.
        Matcher vm = Pattern.compile(
                "\\{\\s*\"x\"\\s*:\\s*(\\d+)\\s*,\\s*\"y\"\\s*:\\s*(\\d+)\\s*,\\s*\"z\"\\s*:\\s*(\\d+)\\s*,\\s*\"mat\"\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"rgb\"\\s*:\\s*(\\d+)(?:\\s*,\\s*\"part\"\\s*:\\s*\"([^\"]+)\")?\\s*\\}")
                .matcher(text);
        while (vm.find()) {
            int x = Integer.parseInt(vm.group(1));
            int y = Integer.parseInt(vm.group(2));
            int z = Integer.parseInt(vm.group(3));
            int mat = MaterialPalette.idFromName(vm.group(4));
            int rgb = Integer.parseInt(vm.group(5));
            int part = vm.group(6) != null ? WeaponParts.idFromName(vm.group(6)) : 0;
            doc.grid.set(x, y, z, mat, rgb, part);
        }
        doc.grid.assertCubicUnitInvariant();
        return doc;
    }

    // ---- v2 run-length layers ----------------------------------------------

    private static void appendRun(StringBuilder sb, int y, int z, int x0, int len, int idx) {
        if (sb.length() > 0) sb.append(',');
        sb.append(y).append(',').append(z).append(',').append(x0).append(',').append(len).append(',').append(idx);
    }

    /** The cell's paint, or -1 when it is air or carries its material's own colour. */
    private static int paintOf(VoxelGrid g, int x, int y, int z) {
        int m = g.getMat(x, y, z);
        if (m == MaterialPalette.AIR) return -1;
        int rgb = g.getRgb(x, y, z) & 0xFFFFFF;
        return rgb == MaterialPalette.defaultRgb(m) ? -1 : rgb;
    }

    private static int nearestColor(List<Integer> colors, int c) {
        int best = 0;
        long bestD = Long.MAX_VALUE;
        for (int i = 0; i < colors.size(); i++) {
            int o = colors.get(i);
            long dr = ((o >> 16) & 255) - ((c >> 16) & 255);
            long dg = ((o >> 8) & 255) - ((c >> 8) & 255);
            long db = (o & 255) - (c & 255);
            long d = dr * dr + dg * dg + db * db;
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    private static String quoted(List<String> names) {
        StringBuilder sb = new StringBuilder("[");
        for (int i = 0; i < names.size(); i++) {
            if (i > 0) sb.append(", ");
            sb.append('"').append(names.get(i)).append('"');
        }
        return sb.append(']').toString();
    }

    /** The {...} body that follows "key": in text, braces balanced; "" when absent. */
    private static String objectBody(String text, String key) {
        int k = text.indexOf("\"" + key + "\"");
        if (k < 0) return "";
        int open = text.indexOf('{', k);
        if (open < 0) return "";
        int depth = 0;
        for (int i = open; i < text.length(); i++) {
            char ch = text.charAt(i);
            if (ch == '{') depth++;
            else if (ch == '}' && --depth == 0) return text.substring(open + 1, i);
        }
        return "";
    }

    /** Every integer inside the [...] that follows "key": (nested arrays flatten). */
    private static int[] intsAfter(String body, String key) {
        int k = body.indexOf("\"" + key + "\"");
        if (k < 0) return new int[0];
        int open = body.indexOf('[', k);
        int depth = 0, close = -1;
        for (int i = open; i >= 0 && i < body.length(); i++) {
            char ch = body.charAt(i);
            if (ch == '[') depth++;
            else if (ch == ']' && --depth == 0) { close = i; break; }
        }
        if (open < 0 || close < 0) return new int[0];
        Matcher m = Pattern.compile("-?\\d+").matcher(body.substring(open, close));
        List<Integer> out = new ArrayList<>();
        while (m.find()) out.add(Integer.parseInt(m.group()));
        int[] a = new int[out.size()];
        for (int i = 0; i < a.length; i++) a[i] = out.get(i);
        return a;
    }

    private static List<String> namesAfter(String body, String key) {
        List<String> out = new ArrayList<>();
        int k = body.indexOf("\"" + key + "\"");
        if (k < 0) return out;
        int open = body.indexOf('[', k), close = body.indexOf(']', open);
        if (open < 0 || close < 0) return out;
        Matcher m = Pattern.compile("\"([^\"]*)\"").matcher(body.substring(open, close));
        while (m.find()) out.add(m.group(1));
        return out;
    }

    private static void loadRunLayers(String text, VoxDocument doc) throws IOException {
        VoxelGrid g = doc.grid;
        String cells = objectBody(text, "cells_rle");
        List<String> mats = namesAfter(cells, "palette");
        int[] runs = intsAfter(cells, "runs");
        if (runs.length % 5 != 0) throw new IOException("cells_rle runs must be quintuples");
        for (int r = 0; r < runs.length; r += 5) {
            int y = runs[r], z = runs[r + 1], x0 = runs[r + 2], len = runs[r + 3], idx = runs[r + 4];
            if (idx < 0 || idx >= mats.size() || len <= 0) throw new IOException("cells_rle run " + r / 5 + " is malformed");
            int m = MaterialPalette.idFromName(mats.get(idx));
            for (int x = x0; x < x0 + len; x++)
                if (g.inBounds(x, y, z)) g.set(x, y, z, m, MaterialPalette.defaultRgb(m));
        }
        String paint = objectBody(text, "appearance");
        int[] pal = intsAfter(paint, "palette");
        int[] pruns = intsAfter(paint, "runs");
        for (int r = 0; r + 4 < pruns.length; r += 5) {
            int idx = pruns[r + 4] - 1;
            if (idx < 0 || idx * 3 + 2 >= pal.length) throw new IOException("appearance run " + r / 5 + " is malformed");
            int rgb = ((pal[idx * 3] & 255) << 16) | ((pal[idx * 3 + 1] & 255) << 8) | (pal[idx * 3 + 2] & 255);
            for (int x = pruns[r + 2]; x < pruns[r + 2] + pruns[r + 3]; x++) {
                int y = pruns[r], z = pruns[r + 1];
                if (g.inBounds(x, y, z) && g.getMat(x, y, z) != MaterialPalette.AIR)
                    g.set(x, y, z, g.getMat(x, y, z), rgb, g.getPart(x, y, z));
            }
        }
        String parts = objectBody(text, "parts_rle");
        List<String> partNames = namesAfter(parts, "palette");
        int[] partRuns = intsAfter(parts, "runs");
        for (int r = 0; r + 4 < partRuns.length; r += 5) {
            int idx = partRuns[r + 4];
            if (idx < 0 || idx >= partNames.size()) throw new IOException("parts_rle run " + r / 5 + " is malformed");
            int part = WeaponParts.idFromName(partNames.get(idx));
            for (int x = partRuns[r + 2]; x < partRuns[r + 2] + partRuns[r + 3]; x++)
                if (g.inBounds(x, partRuns[r], partRuns[r + 1])) g.setPart(x, partRuns[r], partRuns[r + 1], part);
        }
    }

    // ---- MAP mode serialization --------------------------------------------
    // Map entities live in the same .vox.json file as the voxel grid, in three
    // optional sections emitted before "voxels": scripted_events, npcs and
    // patrol_routes. Each section is omitted when its list is empty.

    /** A JSON string value, tolerant of backslash escapes written by appendEscaped. */
    /**
     * One JSON string, capture group included, quotes NOT included in the capture.
     * The trailing quote is part of the pattern: the body alternation stops at an
     * unescaped quote, so without it every string field failed to match.
     */
    private static final String JSON_STR = "\"((?:[^\"\\\\]|\\\\.)*)\"";
    /** A JSON number we emit: optional sign, integer part, optional fraction. */
    private static final String JSON_NUM = "(-?\\d+(?:\\.\\d+)?)";

    /** Append a quoted, escaped JSON string. */
    private static void appendEscaped(StringBuilder sb, String s) {
        if (s == null) { sb.append("\"\""); return; }
        sb.append('"');
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            switch (c) {
                case '"' -> sb.append("\\\"");
                case '\\' -> sb.append("\\\\");
                case '\n' -> sb.append("\\n");
                case '\r' -> sb.append("\\r");
                case '\t' -> sb.append("\\t");
                case '\b' -> sb.append("\\b");
                case '\f' -> sb.append("\\f");
                default -> {
                    if (c < 0x20) sb.append(String.format(Locale.ROOT, "\\u%04x", (int) c));
                    else sb.append(c);
                }
            }
        }
        sb.append('"');
    }

    /** Reverse of appendEscaped, so round-tripped names keep their punctuation. */
    private static String unescape(String s) {
        if (s == null || s.indexOf('\\') < 0) return s;
        StringBuilder sb = new StringBuilder(s.length());
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c != '\\' || i + 1 >= s.length()) { sb.append(c); continue; }
            char n = s.charAt(++i);
            switch (n) {
                case 'n' -> sb.append('\n');
                case 'r' -> sb.append('\r');
                case 't' -> sb.append('\t');
                case 'b' -> sb.append('\b');
                case 'f' -> sb.append('\f');
                case 'u' -> {
                    if (i + 4 < s.length()) {
                        sb.append((char) Integer.parseInt(s.substring(i + 1, i + 5), 16));
                        i += 4;
                    } else {
                        sb.append(n);
                    }
                }
                default -> sb.append(n);
            }
        }
        return sb.toString();
    }

    private static void saveMapEntities(StringBuilder sb, MapEntities.MapData mapData) {
        if (!mapData.events.isEmpty()) {
            sb.append("  \"scripted_events\": [\n");
            List<String> eventRows = new ArrayList<>();
            for (MapEntities.ScriptedEvent e : mapData.events) {
                StringBuilder esb = new StringBuilder();
                esb.append("    {\"x\":").append(e.x)
                        .append(",\"y\":").append(e.y)
                        .append(",\"z\":").append(e.z);
                appendEscaped(esb.append(",\"id\":"), e.id);
                appendEscaped(esb.append(",\"name\":"), e.name);
                appendEscaped(esb.append(",\"script\":"), e.scriptName);
                appendEscaped(esb.append(",\"trigger\":"), e.triggerType);
                esb.append(",\"radius\":").append(String.format(Locale.ROOT, "%.3f", e.radius))
                        .append(",\"cooldown\":").append(e.cooldownTicks);
                appendEscaped(esb.append(",\"required_signal\":"), e.requiredSignal);
                appendEscaped(esb.append(",\"emit_signal\":"), e.emitSignal);
                appendEscaped(esb.append(",\"condition\":"), e.condition);
                esb.append(",\"repeat\":").append(e.repeatLimit)
                        .append(",\"enabled\":").append(e.enabled);
                esb.append("}");
                eventRows.add(esb.toString());
            }
            sb.append(String.join(",\n", eventRows));
            sb.append("\n  ],\n");
        }
        if (!mapData.npcs.isEmpty()) {
            sb.append("  \"npcs\": [\n");
            List<String> npcRows = new ArrayList<>();
            for (MapEntities.Npc n : mapData.npcs) {
                StringBuilder nsb = new StringBuilder();
                nsb.append("    {\"x\":").append(n.x)
                        .append(",\"y\":").append(n.y)
                        .append(",\"z\":").append(n.z);
                appendEscaped(nsb.append(",\"id\":"), n.id);
                appendEscaped(nsb.append(",\"name\":"), n.name);
                appendEscaped(nsb.append(",\"type\":"), n.npcType);
                appendEscaped(nsb.append(",\"ai_profile\":"), n.aiProfile);
                appendEscaped(nsb.append(",\"patrol_route\":"), n.patrolRouteId);
                nsb.append(",\"health\":").append(n.health)
                        .append(",\"max_health\":").append(n.maxHealth)
                        .append(",\"speed\":").append(String.format(Locale.ROOT, "%.3f", n.moveSpeed))
                        .append(",\"view_dist\":").append(String.format(Locale.ROOT, "%.3f", n.viewDistance))
                        .append(",\"view_angle\":").append(String.format(Locale.ROOT, "%.1f", n.viewAngle));
                appendEscaped(nsb.append(",\"faction\":"), n.faction);
                appendEscaped(nsb.append(",\"dialogue\":"), n.dialogueId);
                appendEscaped(nsb.append(",\"inventory\":"), n.inventoryJson);
                nsb.append(",\"static\":").append(n.isStatic)
                        .append(",\"spawn_tick\":").append(n.spawnTick);
                appendEscaped(nsb.append(",\"spawn_condition\":"), n.spawnCondition);
                nsb.append(",\"enabled\":").append(n.enabled);
                nsb.append("}");
                npcRows.add(nsb.toString());
            }
            sb.append(String.join(",\n", npcRows));
            sb.append("\n  ],\n");
        }
        if (!mapData.patrolRoutes.isEmpty()) {
            sb.append("  \"patrol_routes\": [\n");
            List<String> routeRows = new ArrayList<>();
            for (MapEntities.PatrolRoute r : mapData.patrolRoutes) {
                StringBuilder rsb = new StringBuilder();
                appendEscaped(rsb.append("    {\"id\":"), r.id);
                appendEscaped(rsb.append(",\"name\":"), r.name);
                rsb.append(",\"loop\":").append(r.loop);
                rsb.append(",\"nodes\":[");
                List<String> nodeRows = new ArrayList<>();
                for (MapEntities.PatrolNode node : r.nodes) {
                    StringBuilder nodeSb = new StringBuilder();
                    nodeSb.append("{\"x\":").append(node.x)
                            .append(",\"y\":").append(node.y)
                            .append(",\"z\":").append(node.z)
                            .append(",\"wait\":").append(String.format(Locale.ROOT, "%.2f", node.waitTime));
                    appendEscaped(nodeSb.append(",\"action\":"), node.action);
                    nodeSb.append("}");
                    nodeRows.add(nodeSb.toString());
                }
                rsb.append(String.join(",", nodeRows));
                rsb.append("]}");
                routeRows.add(rsb.toString());
            }
            sb.append(String.join(",\n", routeRows));
            sb.append("\n  ],\n");
        }
    }

    private static void loadMapEntities(String text, MapEntities.MapData mapData) {
        Matcher eventMatcher = Pattern.compile(
                "\\{\\s*\"x\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"y\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"z\"\\s*:\\s*(-?\\d+)\\s*,"
                        + "\\s*\"id\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"name\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"script\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"trigger\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"radius\"\\s*:\\s*" + JSON_NUM + "\\s*,"
                        + "\\s*\"cooldown\"\\s*:\\s*(-?\\d+)\\s*,"
                        + "\\s*\"required_signal\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"emit_signal\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"condition\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"repeat\"\\s*:\\s*(-?\\d+)\\s*,"
                        + "\\s*\"enabled\"\\s*:\\s*(true|false)\\s*\\}")
                .matcher(text);
        while (eventMatcher.find()) {
            MapEntities.ScriptedEvent e = new MapEntities.ScriptedEvent(
                    Integer.parseInt(eventMatcher.group(1)),
                    Integer.parseInt(eventMatcher.group(2)),
                    Integer.parseInt(eventMatcher.group(3)),
                    unescape(eventMatcher.group(4)),
                    unescape(eventMatcher.group(5)),
                    unescape(eventMatcher.group(6)));
            e.triggerType = unescape(eventMatcher.group(7));
            e.radius = Float.parseFloat(eventMatcher.group(8));
            e.cooldownTicks = Integer.parseInt(eventMatcher.group(9));
            e.requiredSignal = unescape(eventMatcher.group(10));
            e.emitSignal = unescape(eventMatcher.group(11));
            e.condition = unescape(eventMatcher.group(12));
            e.repeatLimit = Integer.parseInt(eventMatcher.group(13));
            e.enabled = Boolean.parseBoolean(eventMatcher.group(14));
            mapData.addEvent(e);
        }

        Matcher npcMatcher = Pattern.compile(
                "\\{\\s*\"x\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"y\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"z\"\\s*:\\s*(-?\\d+)\\s*,"
                        + "\\s*\"id\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"name\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"type\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"ai_profile\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"patrol_route\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"health\"\\s*:\\s*(-?\\d+)\\s*,"
                        + "\\s*\"max_health\"\\s*:\\s*(-?\\d+)\\s*,"
                        + "\\s*\"speed\"\\s*:\\s*" + JSON_NUM + "\\s*,"
                        + "\\s*\"view_dist\"\\s*:\\s*" + JSON_NUM + "\\s*,"
                        + "\\s*\"view_angle\"\\s*:\\s*" + JSON_NUM + "\\s*,"
                        + "\\s*\"faction\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"dialogue\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"inventory\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"static\"\\s*:\\s*(true|false)\\s*,"
                        + "\\s*\"spawn_tick\"\\s*:\\s*(-?\\d+)\\s*,"
                        + "\\s*\"spawn_condition\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"enabled\"\\s*:\\s*(true|false)\\s*\\}")
                .matcher(text);
        while (npcMatcher.find()) {
            MapEntities.Npc n = new MapEntities.Npc(
                    Integer.parseInt(npcMatcher.group(1)),
                    Integer.parseInt(npcMatcher.group(2)),
                    Integer.parseInt(npcMatcher.group(3)),
                    unescape(npcMatcher.group(4)),
                    unescape(npcMatcher.group(5)),
                    unescape(npcMatcher.group(6)));
            n.aiProfile = unescape(npcMatcher.group(7));
            n.patrolRouteId = unescape(npcMatcher.group(8));
            n.health = Integer.parseInt(npcMatcher.group(9));
            n.maxHealth = Integer.parseInt(npcMatcher.group(10));
            n.moveSpeed = Float.parseFloat(npcMatcher.group(11));
            n.viewDistance = Float.parseFloat(npcMatcher.group(12));
            n.viewAngle = Float.parseFloat(npcMatcher.group(13));
            n.faction = unescape(npcMatcher.group(14));
            n.dialogueId = unescape(npcMatcher.group(15));
            n.inventoryJson = unescape(npcMatcher.group(16));
            n.isStatic = Boolean.parseBoolean(npcMatcher.group(17));
            n.spawnTick = Integer.parseInt(npcMatcher.group(18));
            n.spawnCondition = unescape(npcMatcher.group(19));
            n.enabled = Boolean.parseBoolean(npcMatcher.group(20));
            n.currentHealth = n.health;
            mapData.addNpc(n);
        }

        Matcher routeMatcher = Pattern.compile(
                "\\{\\s*\"id\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"name\"\\s*:\\s*" + JSON_STR + "\\s*,"
                        + "\\s*\"loop\"\\s*:\\s*(true|false)\\s*,"
                        + "\\s*\"nodes\"\\s*:\\s*\\[(.*?)\\]\\s*\\}")
                .matcher(text);
        while (routeMatcher.find()) {
            MapEntities.PatrolRoute route = new MapEntities.PatrolRoute(
                    unescape(routeMatcher.group(1)), unescape(routeMatcher.group(2)));
            route.loop = Boolean.parseBoolean(routeMatcher.group(3));
            Matcher nodeMatcher = Pattern.compile(
                    "\\{\\s*\"x\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"y\"\\s*:\\s*(-?\\d+)\\s*,"
                            + "\\s*\"z\"\\s*:\\s*(-?\\d+)\\s*,"
                            + "\\s*\"wait\"\\s*:\\s*" + JSON_NUM + "\\s*,"
                            + "\\s*\"action\"\\s*:\\s*" + JSON_STR + "\\s*\\}")
                    .matcher(routeMatcher.group(4));
            while (nodeMatcher.find()) {
                route.addNode(new MapEntities.PatrolNode(
                        Integer.parseInt(nodeMatcher.group(1)),
                        Integer.parseInt(nodeMatcher.group(2)),
                        Integer.parseInt(nodeMatcher.group(3)),
                        Float.parseFloat(nodeMatcher.group(4)),
                        unescape(nodeMatcher.group(5))));
            }
            mapData.addPatrolRoute(route);
        }
    }

    private static String findString(String text, String key, String def) {
        Matcher m = Pattern.compile("\"" + key + "\"\\s*:\\s*\"([^\"]+)\"").matcher(text);
        return m.find() ? m.group(1) : def;
    }

    private static int findInt(String text, String key, int def) {
        Matcher m = Pattern.compile("\"" + key + "\"\\s*:\\s*(-?\\d+)").matcher(text);
        return m.find() ? Integer.parseInt(m.group(1)) : def;
    }

    private static float findFloat(String text, String key, float def) {
        Matcher m = Pattern.compile("\"" + key + "\"\\s*:\\s*(-?\\d+(?:\\.\\d+)?(?:[eE][-+]?\\d+)?)").matcher(text);
        return m.find() ? Float.parseFloat(m.group(1)) : def;
    }

    private static int[] findIntArray(String text, String key, int[] def) {
        Matcher m = Pattern.compile("\"" + key + "\"\\s*:\\s*\\[\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*\\]").matcher(text);
        if (!m.find()) return def;
        return new int[]{Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2)), Integer.parseInt(m.group(3))};
    }

    private static float[] findFloatArray(String text, String key, float[] def) {
        Matcher m = Pattern.compile(
                "\"" + key + "\"\\s*:\\s*\\[\\s*(-?\\d+(?:\\.\\d+)?)\\s*,\\s*(-?\\d+(?:\\.\\d+)?)\\s*,\\s*(-?\\d+(?:\\.\\d+)?)\\s*\\]")
                .matcher(text);
        if (!m.find()) return def;
        return new float[]{Float.parseFloat(m.group(1)), Float.parseFloat(m.group(2)), Float.parseFloat(m.group(3))};
    }
}
