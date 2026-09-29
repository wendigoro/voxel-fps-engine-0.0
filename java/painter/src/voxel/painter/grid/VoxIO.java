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
                    if (c < 0x20) sb.append(String.format("\\u%04x", (int)c));
                    else sb.append(c);
                }
            }
        }
        sb.append('"');
    }

    public static void save(VoxDocument doc, Path path) throws IOException {
        doc.grid.assertCubicUnitInvariant();
        StringBuilder sb = new StringBuilder();
        sb.append("{\n");
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
        if (doc.mode == VoxDocument.Mode.CHARACTER) {
            sb.append("  \"feet\": [").append(doc.feetX).append(", ")
                    .append(doc.feetY).append(", ").append(doc.feetZ).append("],\n");
        }
        if (doc.mode == VoxDocument.Mode.WEAPON) {
            sb.append("  \"caliber\": \"").append(doc.caliber).append("\",\n");
            sb.append("  \"ammo_id\": \"").append(doc.ammoId).append("\",\n");
        }
        if (doc.mode == VoxDocument.Mode.MAP) {
            saveMapEntities(sb, doc.mapData);
        }
        sb.append("  \"voxels\": [\n");
        List<String> rows = new ArrayList<>();
        VoxelGrid g = doc.grid;
        for (int y = 0; y < g.sizeY(); y++)
            for (int z = 0; z < g.sizeZ(); z++)
                for (int x = 0; x < g.sizeX(); x++) {
                    int m = g.getMat(x, y, z);
                    if (m == MaterialPalette.AIR && g.getRgb(x, y, z) == 0) continue;
                    int rgb = g.getRgb(x, y, z);
                    int part = g.getPart(x, y, z);
                    if (part > 0) {
                        rows.add(String.format(Locale.ROOT,
                                "    {\"x\":%d,\"y\":%d,\"z\":%d,\"mat\":\"%s\",\"rgb\":%d,\"part\":\"%s\"}",
                                x, y, z, MaterialPalette.nameFromId(m), rgb, WeaponParts.name(part)));
                    } else {
                        rows.add(String.format(Locale.ROOT,
                                "    {\"x\":%d,\"y\":%d,\"z\":%d,\"mat\":\"%s\",\"rgb\":%d}",
                                x, y, z, MaterialPalette.nameFromId(m), rgb));
                    }
                }
        sb.append(String.join(",\n", rows));
        if (!rows.isEmpty()) sb.append('\n');
        sb.append("  ]\n}\n");
        Files.createDirectories(path.getParent() == null ? Path.of(".") : path.getParent());
        Files.writeString(path, sb.toString(), StandardCharsets.UTF_8);
    }

    private static void saveMapEntities(StringBuilder sb, MapEntities.MapData mapData) {
        // Scripted events
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
        // NPCs
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
        // Patrol routes
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

public static VoxDocument load(Path path) throws IOException {
        String text = Files.readString(path, StandardCharsets.UTF_8);
        String mode = findString(text, "mode", "model");
        int[] dims = findIntArray(text, "dims", new int[]{16, 16, 16});
        VoxDocument doc = new VoxDocument(VoxDocument.parseMode(mode), dims[0], dims[1], dims[2]);
        doc.segU = findInt(text, "seg_u", 28);
        doc.segV = findInt(text, "seg_v", 14);
        float[] moon = findFloatArray(text, "moon_dir", new float[]{0.32f, 0.82f, -0.48f});
        doc.moonDirX = moon[0]; doc.moonDirY = moon[1]; doc.moonDirZ = moon[2];
        doc.moonIntensity = findFloat(text, "moon_intensity", 0.95f);
        int[] feet = findIntArray(text, "feet", new int[]{0, 0, 0});
        doc.feetX = feet[0]; doc.feetY = feet[1]; doc.feetZ = feet[2];
        doc.caliber = findString(text, "caliber", doc.caliber);
        doc.ammoId = findString(text, "ammo_id", doc.ammoId);

        // Load map entities if map mode
        if (doc.mode == VoxDocument.Mode.MAP) {
            loadMapEntities(text, doc.mapData);
        }

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

    private static void loadMapEntities(String text, MapEntities.MapData mapData) {
        // Load scripted events
        Matcher eventMatcher = Pattern.compile(
                "\\{\\s*\"x\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"y\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"z\"\\s*:\\s*(-?\\d+)\\s*," +
                "\\s*\"id\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"name\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"script\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"trigger\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"radius\"\\s*:\\s*([\\d.]+)\\s*,\\s*\"cooldown\"\\s*:\\s*(-?\\d+)\\s*," +
                "\\s*\"required_signal\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"emit_signal\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"condition\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"repeat\"\\s*:\\s*(-?\\d+)\\s*," +
                "\\s*\"enabled\"\\s*:\\s*(true|false)\\s*\\}"
        ).matcher(text);
        while (eventMatcher.find()) {
            MapEntities.ScriptedEvent e = new MapEntities.ScriptedEvent(
                    Integer.parseInt(eventMatcher.group(1)),
                    Integer.parseInt(eventMatcher.group(2)),
                    Integer.parseInt(eventMatcher.group(3)),
                    eventMatcher.group(4), eventMatcher.group(5), eventMatcher.group(6));
            e.triggerType = eventMatcher.group(7);
            e.radius = Float.parseFloat(eventMatcher.group(8));
            e.cooldownTicks = Integer.parseInt(eventMatcher.group(9));
            e.requiredSignal = eventMatcher.group(10);
            e.emitSignal = eventMatcher.group(11);
            e.condition = eventMatcher.group(12);
            e.repeatLimit = Integer.parseInt(eventMatcher.group(13));
            e.enabled = Boolean.parseBoolean(eventMatcher.group(14));
            mapData.addEvent(e);
        }

        // Load NPCs
        Matcher npcMatcher = Pattern.compile(
                "\\{\\s*\"x\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"y\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"z\"\\s*:\\s*(-?\\d+)\\s*," +
                "\\s*\"id\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"name\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"type\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"ai_profile\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"patrol_route\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"health\"\\s*:\\s*(-?\\d+)\\s*," +
                "\\s*\"max_health\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"speed\"\\s*:\\s*([\\d.]+)\\s*," +
                "\\s*\"view_dist\"\\s*:\\s*([\\d.]+)\\s*,\\s*\"view_angle\"\\s*:\\s*([\\d.]+)\\s*," +
                "\\s*\"faction\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"dialogue\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"inventory\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"static\"\\s*:\\s*(true|false)\\s*," +
                "\\s*\"spawn_tick\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"spawn_condition\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"enabled\"\\s*:\\s*(true|false)\\s*\\}"
        ).matcher(text);
        while (npcMatcher.find()) {
            MapEntities.Npc n = new MapEntities.Npc(
                    Integer.parseInt(npcMatcher.group(1)),
                    Integer.parseInt(npcMatcher.group(2)),
                    Integer.parseInt(npcMatcher.group(3)),
                    npcMatcher.group(4), npcMatcher.group(5), npcMatcher.group(6));
            n.aiProfile = npcMatcher.group(7);
            n.patrolRouteId = npcMatcher.group(8);
            n.health = Integer.parseInt(npcMatcher.group(9));
            n.maxHealth = Integer.parseInt(npcMatcher.group(10));
            n.moveSpeed = Float.parseFloat(npcMatcher.group(11));
            n.viewDistance = Float.parseFloat(npcMatcher.group(12));
            n.viewAngle = Float.parseFloat(npcMatcher.group(13));
            n.faction = npcMatcher.group(14);
            n.dialogueId = npcMatcher.group(15);
            n.inventoryJson = npcMatcher.group(16);
            n.isStatic = Boolean.parseBoolean(npcMatcher.group(17));
            n.spawnTick = Integer.parseInt(npcMatcher.group(18));
            n.spawnCondition = npcMatcher.group(19);
            n.enabled = Boolean.parseBoolean(npcMatcher.group(20));
            n.currentHealth = n.health;
            mapData.addNpc(n);
        }

        // Load patrol routes
        Matcher routeMatcher = Pattern.compile(
                "\\{\\s*\"id\"\\s*:\\s*\"([^\"]*)\"\\s*,\\s*\"name\"\\s*:\\s*\"([^\"]*)\"\\s*," +
                "\\s*\"loop\"\\s*:\\s*(true|false)\\s*,\\s*\"nodes\"\\s*:\\s*\\[([^\\]]*)\\]\\s*\\}"
        ).matcher(text);
        while (routeMatcher.find()) {
            MapEntities.PatrolRoute route = new MapEntities.PatrolRoute(routeMatcher.group(1), routeMatcher.group(2));
            route.loop = Boolean.parseBoolean(routeMatcher.group(3));
            String nodesText = routeMatcher.group(4);
            // Parse nodes
            Matcher nodeMatcher = Pattern.compile(
                    "\\{\\s*\"x\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"y\"\\s*:\\s*(-?\\d+)\\s*,\\s*\"z\"\\s*:\\s*(-?\\d+)\\s*," +
                    "\\s*\"wait\"\\s*:\\s*([\\d.]+)\\s*,\\s*\"action\"\\s*:\\s*\"([^\"]*)\"\\s*\\}"
            ).matcher(nodesText);
            while (nodeMatcher.find()) {
                MapEntities.PatrolNode node = new MapEntities.PatrolNode(
                        Integer.parseInt(nodeMatcher.group(1)),
                        Integer.parseInt(nodeMatcher.group(2)),
                        Integer.parseInt(nodeMatcher.group(3)),
                        Float.parseFloat(nodeMatcher.group(4)),
                        nodeMatcher.group(5));
                route.addNode(node);
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
