package voxel.painter.grid;

import java.io.BufferedReader;
import java.io.File;
import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;

/**
 * Bridge between Java Painter and the local Blender instance.
 * Allows querying available 3D mesh objects from .blend projects and fetching
 * them directly into in-memory VoxDocument / VoxelGrid structures.
 */
public final class BlenderBridge {
    private BlenderBridge() {}

    private static final String[] BLENDER_CANDIDATE_PATHS = {
        "C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe",
        "C:\\Program Files\\Blender Foundation\\Blender 5.1\\blender.exe",
        "C:\\Program Files\\Blender Foundation\\Blender 5.0\\blender.exe",
        "C:\\Program Files\\Blender Foundation\\Blender 4.2\\blender.exe",
        "C:\\Program Files\\Blender Foundation\\Blender\\blender.exe"
    };

    /** Locates the installed blender.exe executable on the host system. */
    public static Path findBlenderExecutable() {
        String custom = System.getProperty("blender.executable");
        if (custom != null && !custom.isBlank() && Files.isExecutable(Path.of(custom))) {
            return Path.of(custom);
        }
        String env = System.getenv("BLENDER_BIN");
        if (env != null && !env.isBlank() && Files.isExecutable(Path.of(env))) {
            return Path.of(env);
        }
        for (String p : BLENDER_CANDIDATE_PATHS) {
            Path path = Path.of(p);
            if (Files.exists(path) && Files.isExecutable(path)) {
                return path;
            }
        }
        return null;
    }

    /** Returns true if a local Blender binary was discovered. */
    public static boolean isBlenderAvailable() {
        return findBlenderExecutable() != null;
    }

    /** Returns the path to the canonical voxel rig and models .blend file. */
    public static Path defaultBlendPath(Path repoRoot) {
        if (repoRoot != null) {
            Path repoBlend = repoRoot.resolve("data").resolve("blender").resolve("voxel_rig_and_models.blend");
            if (Files.exists(repoBlend)) return repoBlend;
        }
        String appdata = System.getenv("APPDATA");
        if (appdata != null) {
            Path userBlend = Path.of(appdata, "Blender Foundation", "Blender", "5.2", "voxel_rig_and_models.blend");
            if (Files.exists(userBlend)) return userBlend;
        }
        return null;
    }

    /** Lists mesh objects present inside a .blend project. */
    public static List<String> listObjects(Path repoRoot, Path blendPath) {
        List<String> objects = new ArrayList<>();
        Path blender = findBlenderExecutable();
        Path script = repoRoot != null ? repoRoot.resolve("tools").resolve("blender").resolve("fetch_blend.py") : null;

        if (blender != null && script != null && Files.exists(script) && blendPath != null && Files.exists(blendPath)) {
            try {
                Path tempOut = Files.createTempFile("blend_objs_", ".json");
                ProcessBuilder pb = new ProcessBuilder(
                    blender.toString(),
                    blendPath.toString(),
                    "--background",
                    "--python", script.toString(),
                    "--",
                    "--list",
                    "--out", tempOut.toString()
                );
                pb.redirectErrorStream(true);
                Process proc = pb.start();
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
                    String line;
                    while ((line = reader.readLine()) != null) {
                        if (line.startsWith("MESH_OBJECTS:")) {
                            String listPart = line.substring("MESH_OBJECTS:".length()).trim();
                            for (String item : listPart.split(",")) {
                                String trimmed = item.trim();
                                if (!trimmed.isEmpty()) objects.add(trimmed);
                            }
                        }
                    }
                }
                proc.waitFor();
                try { Files.deleteIfExists(tempOut); } catch (Exception ignored) {}
            } catch (Exception e) {
                System.err.println("[BlenderBridge] Object listing error: " + e.getMessage());
            }
        }

        if (objects.isEmpty()) {
            // Known in-game objects fallback
            objects.addAll(List.of(
                "starter_rifle", "Voxel_Character_Mesh", "shotgun_light",
                "carbine_energy", "sidearm_light", "smg_light", "marksman_rifle",
                "lmg_medium", "sniper_heavy", "cannon_heavy", "railgun_energy"
            ));
        }
        return objects;
    }

    /**
     * Extracts a named mesh object from a .blend project into a VoxDocument.
     * Executes fetch_blend.py headlessly, then deserializes via VoxIO.load().
     */
    public static VoxDocument fetchObject(Path repoRoot, Path blendPath, String objectName) throws IOException {
        Path blender = findBlenderExecutable();
        Path script = repoRoot != null ? repoRoot.resolve("tools").resolve("blender").resolve("fetch_blend.py") : null;

        if (blendPath == null) {
            blendPath = defaultBlendPath(repoRoot);
        }

        if (blender != null && script != null && Files.exists(script) && blendPath != null && Files.exists(blendPath)) {
            Path tempOut = Files.createTempFile("fetch_blend_", ".vox.json");
            try {
                List<String> cmd = new ArrayList<>();
                cmd.add(blender.toString());
                cmd.add(blendPath.toString());
                cmd.add("--background");
                cmd.add("--python");
                cmd.add(script.toString());
                cmd.add("--");
                if (objectName != null && !objectName.isBlank()) {
                    cmd.add("--object");
                    cmd.add(objectName);
                }
                cmd.add("--out");
                cmd.add(tempOut.toString());

                ProcessBuilder pb = new ProcessBuilder(cmd);
                pb.redirectErrorStream(true);
                Process proc = pb.start();

                StringBuilder logs = new StringBuilder();
                try (BufferedReader reader = new BufferedReader(new InputStreamReader(proc.getInputStream()))) {
                    String line;
                    while ((line = reader.readLine()) != null) {
                        logs.append(line).append('\n');
                    }
                }
                int exitCode = proc.waitFor();
                if (exitCode == 0 && Files.exists(tempOut) && Files.size(tempOut) > 0) {
                    VoxDocument doc = VoxIO.load(tempOut);
                    doc.grid.assertCubicUnitInvariant();
                    return doc;
                }
                System.err.println("[BlenderBridge] Blender returned " + exitCode + ": " + logs);
            } catch (InterruptedException ie) {
                Thread.currentThread().interrupt();
                throw new IOException("Blender fetch interrupted", ie);
            } finally {
                try { Files.deleteIfExists(tempOut); } catch (Exception ignored) {}
            }
        }

        // Resilient fallback for environments without active Blender: load corresponding voxfmt file
        if (repoRoot != null && objectName != null) {
            Path fallbackPath = repoRoot.resolve("data").resolve("voxfmt").resolve(objectName + ".vox.json");
            if (Files.exists(fallbackPath)) {
                return VoxIO.load(fallbackPath);
            }
        }

        throw new IOException("Unable to fetch object '" + objectName + "' from Blender project: " + blendPath);
    }
}
