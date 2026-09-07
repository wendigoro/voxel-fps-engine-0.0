package voxel.painter.ui;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.function.Consumer;

/** Shells out to repo scripts via PowerShell ProcessBuilder when present. */
public final class ScriptRunner {
    private final Path repoRoot;
    private final Consumer<String> log;

    public ScriptRunner(Path repoRoot, Consumer<String> log) {
        this.repoRoot = repoRoot;
        this.log = log == null ? s -> {} : log;
    }

    public Path repoRoot() {
        return repoRoot;
    }

    public boolean scriptExists(String relative) {
        return Files.isRegularFile(repoRoot.resolve(relative));
    }

    public CompletableFuture<Integer> runScript(String relative, String... extraArgs) {
        Path script = repoRoot.resolve(relative);
        if (!Files.isRegularFile(script)) {
            log.accept("Script not found: " + script);
            return CompletableFuture.completedFuture(-1);
        }
        List<String> cmd = new ArrayList<>();
        cmd.add("powershell");
        cmd.add("-NoProfile");
        cmd.add("-ExecutionPolicy");
        cmd.add("Bypass");
        cmd.add("-File");
        cmd.add(script.toAbsolutePath().toString());
        if (extraArgs != null) {
            for (String a : extraArgs) {
                if (a != null && !a.isBlank()) cmd.add(a);
            }
        }
        log.accept("Running: " + String.join(" ", cmd));
        return CompletableFuture.supplyAsync(() -> {
            try {
                ProcessBuilder pb = new ProcessBuilder(cmd);
                pb.directory(repoRoot.toFile());
                pb.redirectErrorStream(true);
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
                log.accept("Failed: " + ex.getMessage());
                return -1;
            }
        });
    }
}
