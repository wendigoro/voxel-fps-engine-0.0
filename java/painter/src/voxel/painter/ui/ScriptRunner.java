package voxel.painter.ui;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

/** Runs repo build/smoke actions via scripts/painter_dev.cmd (path-contained). */
public final class ScriptRunner {
    public static final String BRIDGE_CMD = "scripts/painter_dev.cmd";
    public static final String BRIDGE_PS1 = "scripts/painter_dev.ps1";

    private final Path repoRoot;
    private final Consumer<String> log;
    private final AtomicBoolean busy = new AtomicBoolean(false);

    public ScriptRunner(Path repoRoot, Consumer<String> log) {
        this.repoRoot = resolveRepoRoot(repoRoot);
        this.log = log == null ? s -> {} : log;
    }

    public Path repoRoot() { return repoRoot; }
    public boolean isBusy() { return busy.get(); }

    public boolean bridgeExists() {
        return Files.isRegularFile(repoRoot.resolve(BRIDGE_CMD))
                || Files.isRegularFile(repoRoot.resolve(BRIDGE_PS1));
    }

    public boolean scriptExists(String relative) {
        if (relative == null) return false;
        return Files.isRegularFile(repoRoot.resolve(relative.replace('/', '\\')))
                || Files.isRegularFile(repoRoot.resolve(relative));
    }

    public enum Action {
        BUILD_ENGINE("BuildEngine", "Export defs + compile voxel_engine.exe"),
        BUILD_PAINTER("BuildPainter", "javac painter + SmokeMain"),
        SMOKE_ENGINE("SmokeEngine", "Build + engine --smoke"),
        SMOKE_PAINTER("SmokePainter", "Painter headless smoke"),
        SMOKE_ALL("SmokeAll", "Painter smoke then engine smoke"),
        RUN_ENGINE("RunEngine", "Launch interactive engine"),
        HELP("Help", "List actions");

        public final String id;
        public final String label;
        Action(String id, String label) { this.id = id; this.label = label; }
    }

    public static Path resolveRepoRoot(Path hint) {
        Path p = (hint == null ? Path.of(".") : hint).toAbsolutePath().normalize();
        for (int i = 0; i < 8; i++) {
            if (looksLikeRepo(p)) return p;
            Path parent = p.getParent();
            if (parent == null || parent.equals(p)) break;
            p = parent;
        }
        Path cwd = Path.of(".").toAbsolutePath().normalize();
        if (looksLikeRepo(cwd)) return cwd;
        return (hint == null ? cwd : hint.toAbsolutePath().normalize());
    }

    private static boolean looksLikeRepo(Path p) {
        return Files.isRegularFile(p.resolve("launch.ps1"))
                && Files.isRegularFile(p.resolve("scripts").resolve("build.ps1"))
                && Files.isDirectory(p.resolve("java").resolve("painter").resolve("src"));
    }

    public CompletableFuture<Integer> runAction(Action action) {
        return runAction(action == null ? Action.HELP.id : action.id);
    }

    public CompletableFuture<Integer> runAction(String actionId) {
        if (!busy.compareAndSet(false, true)) {
            log.accept("Dev action already running - wait for Exit code.");
            return CompletableFuture.completedFuture(-2);
        }
        final String action = (actionId == null || actionId.isBlank()) ? "Help" : actionId.trim();
        log.accept("---- painter dev ----");
        log.accept("repoRoot = " + repoRoot);
        log.accept("action   = " + action);

        Path bridgeCmd = repoRoot.resolve(BRIDGE_CMD);
        Path bridgePs1 = repoRoot.resolve(BRIDGE_PS1);
        if (!Files.isRegularFile(bridgeCmd) && !Files.isRegularFile(bridgePs1)) {
            busy.set(false);
            log.accept("Missing bridge scripts under " + repoRoot.resolve("scripts"));
            return CompletableFuture.completedFuture(-1);
        }

        List<String> cmd = buildCommand(bridgeCmd, bridgePs1, action);
        log.accept("Running: " + String.join(" ", quoteForLog(cmd)));

        return CompletableFuture.supplyAsync(() -> {
            try {
                ProcessBuilder pb = new ProcessBuilder(cmd);
                pb.directory(repoRoot.toFile());
                pb.redirectErrorStream(true);
                String path = pb.environment().getOrDefault("PATH", "");
                String sys = System.getenv("SystemRoot");
                if (sys == null) sys = "C:\\Windows";
                pb.environment().put("PATH",
                        sys + "\\System32;" + sys + "\\System32\\WindowsPowerShell\\v1.0;" + path);
                pb.environment().put("VOXEL_ENGINE_ROOT", repoRoot.toString());
                Process p = pb.start();
                try (BufferedReader br = new BufferedReader(
                        new InputStreamReader(p.getInputStream(), StandardCharsets.UTF_8))) {
                    String line;
                    while ((line = br.readLine()) != null) log.accept(line);
                }
                int code = p.waitFor();
                log.accept("Exit code: " + code);
                return code;
            } catch (Exception ex) {
                log.accept("Failed: " + ex.getClass().getSimpleName() + ": " + ex.getMessage());
                log.accept("Hint: .\\scripts\\painter_dev.cmd -Action " + action);
                return -1;
            } finally {
                busy.set(false);
            }
        });
    }

    private List<String> buildCommand(Path bridgeCmd, Path bridgePs1, String action) {
        List<String> cmd = new ArrayList<>();
        String sys = System.getenv("SystemRoot");
        if (sys == null || sys.isBlank()) sys = "C:\\Windows";
        Path cmdExe = Path.of(sys, "System32", "cmd.exe");

        if (Files.isRegularFile(bridgeCmd) && Files.isRegularFile(cmdExe)) {
            cmd.add(cmdExe.toString());
            cmd.add("/d");
            cmd.add("/c");
            cmd.add(bridgeCmd.toAbsolutePath().toString());
            cmd.add("-Action");
            cmd.add(action);
            return cmd;
        }

        Path ps = Path.of(sys, "System32", "WindowsPowerShell", "v1.0", "powershell.exe");
        if (!Files.isRegularFile(ps)) ps = Path.of("powershell.exe");
        Path script = Files.isRegularFile(bridgePs1) ? bridgePs1 : bridgeCmd;
        cmd.add(ps.toString());
        cmd.add("-NoLogo");
        cmd.add("-NoProfile");
        cmd.add("-ExecutionPolicy");
        cmd.add("RemoteSigned");
        cmd.add("-File");
        cmd.add(script.toAbsolutePath().toString());
        cmd.add("-Action");
        cmd.add(action);
        return cmd;
    }

    public CompletableFuture<Integer> runScript(String relative, String... extraArgs) {
        String rel = relative == null ? "" : relative.replace('\\', '/').toLowerCase(Locale.ROOT);
        if (rel.endsWith("build.ps1")) return runAction(Action.BUILD_ENGINE);
        if (rel.endsWith("build_painter.ps1")) return runAction(Action.BUILD_PAINTER);
        if (rel.endsWith("smoke_painter.ps1")) return runAction(Action.SMOKE_PAINTER);
        if (rel.endsWith("run.ps1")) return runAction(Action.RUN_ENGINE);
        if (rel.endsWith("demo.ps1")) return runAction(Action.SMOKE_ENGINE);
        if (rel.endsWith("painter_dev.ps1") || rel.endsWith("painter_dev.cmd")) {
            String act = "Help";
            if (extraArgs != null) {
                for (int i = 0; i < extraArgs.length; i++) {
                    if ("-Action".equalsIgnoreCase(extraArgs[i]) && i + 1 < extraArgs.length) {
                        act = extraArgs[i + 1];
                        break;
                    }
                }
            }
            return runAction(act);
        }
        log.accept("Unmapped script '" + relative + "' - use Dev actions.");
        return runAction(Action.HELP);
    }

    private static List<String> quoteForLog(List<String> cmd) {
        List<String> out = new ArrayList<>(cmd.size());
        for (String s : cmd) {
            if (s.indexOf(' ') >= 0) out.add("\"" + s + "\"");
            else out.add(s);
        }
        return out;
    }
}