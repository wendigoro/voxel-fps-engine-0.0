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
