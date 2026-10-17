package voxel.painter.anim;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * In-memory representation of a Voxel FPS Engine animation clip (.anim.json).
 * Conforms to data/anims/schema.md format version 1.
 */
public final class AnimationClip {
    public int formatVersion = 1;
    public String id = "clip";
    public String name = "Animation Clip";
    public double durationSec = 1.0;
    public boolean loop = true;
    public double frameRate = 30.0;
    public final List<Track> tracks = new ArrayList<>();

    public static final class Keyframe {
        public double time;
        public double[] pos = new double[]{0, 0, 0};
        public double[] rot = new double[]{0, 0, 0, 1};

        public Keyframe() {}

        public Keyframe(double time, double[] pos, double[] rot) {
            this.time = time;
            this.pos = pos != null ? pos.clone() : new double[]{0, 0, 0};
            this.rot = rot != null ? rot.clone() : new double[]{0, 0, 0, 1};
        }

        public Keyframe copy() {
            return new Keyframe(time, pos, rot);
        }
    }

    public static final class Track {
        public String bone = "";
        public final List<Keyframe> frames = new ArrayList<>();

        public Track() {}

        public Track(String bone) {
            this.bone = bone != null ? bone : "";
        }

        public Track copy() {
            Track t = new Track(bone);
            for (Keyframe f : frames) {
                t.frames.add(f.copy());
            }
            return t;
        }
    }

    public AnimationClip copy() {
        AnimationClip c = new AnimationClip();
        c.formatVersion = this.formatVersion;
        c.id = this.id;
        c.name = this.name;
        c.durationSec = this.durationSec;
        c.loop = this.loop;
        c.frameRate = this.frameRate;
        for (Track t : this.tracks) {
            c.tracks.add(t.copy());
        }
        return c;
    }

    /**
     * Applies modifiers and returns a new transformed AnimationClip.
     *
     * @param speed Playback speed multiplier (0.1x to 3.0x). Scales duration and keyframe times.
     * @param motionScale Translation motion scale / amplitude (0.1x to 2.0x).
     * @param additiveWeight Additive blending weight (0.0 to 1.0). Slerps rotations toward identity.
     * @param crossFadeSec Cross-fade time metadata in seconds.
     * @param fps Target sample frame rate.
     * @param loopOverride If not null, overrides loop flag.
     * @param idOverride If not blank, overrides clip id.
     * @param nameOverride If not blank, overrides descriptive name.
     * @param targetRig Filter by target rig ("All Bones", "Humanoid Only", "Weapon Only").
     */
    public AnimationClip applyModifiers(double speed, double motionScale, double additiveWeight,
                                        double crossFadeSec, double fps, Boolean loopOverride,
                                        String idOverride, String nameOverride, String targetRig) {
        AnimationClip out = new AnimationClip();
        out.formatVersion = this.formatVersion;
        out.id = (idOverride != null && !idOverride.isBlank()) ? idOverride.trim() : this.id;
        out.name = (nameOverride != null && !nameOverride.isBlank()) ? nameOverride.trim() : this.name;
        double effSpeed = Math.max(0.05, speed);
        out.durationSec = Math.max(0.01, this.durationSec / effSpeed);
        out.frameRate = fps > 0 ? fps : this.frameRate;
        out.loop = loopOverride != null ? loopOverride : this.loop;

        double weight = Math.max(0.0, Math.min(1.0, additiveWeight));

        for (Track t : this.tracks) {
            boolean isWeapon = t.bone != null && t.bone.toLowerCase(Locale.ROOT).startsWith("weapon");
            if ("Humanoid Only".equalsIgnoreCase(targetRig) && isWeapon) continue;
            if ("Weapon Only".equalsIgnoreCase(targetRig) && !isWeapon) continue;

            Track nt = new Track(t.bone);
            for (Keyframe k : t.frames) {
                double newTime = k.time / effSpeed;
                double[] newPos = new double[3];
                for (int i = 0; i < 3; i++) {
                    newPos[i] = k.pos[i] * motionScale * weight;
                }
                double[] newRot = slerp(new double[]{0, 0, 0, 1}, k.rot, weight);
                nt.frames.add(new Keyframe(newTime, newPos, newRot));
            }
            out.tracks.add(nt);
        }
        return out;
    }

    /** Spherical linear interpolation between two quaternions [x, y, z, w]. */
    public static double[] slerp(double[] q0, double[] q1, double t) {
        if (t >= 1.0) return q1.clone();
        if (t <= 0.0) return q0.clone();
        double dot = q0[0]*q1[0] + q0[1]*q1[1] + q0[2]*q1[2] + q0[3]*q1[3];
        double[] q1Mod = q1.clone();
        if (dot < 0.0) {
            dot = -dot;
            for (int i = 0; i < 4; i++) q1Mod[i] = -q1Mod[i];
        }
        if (dot > 0.9995) {
            double[] res = new double[4];
            double lenSq = 0;
            for (int i = 0; i < 4; i++) {
                res[i] = q0[i] + t * (q1Mod[i] - q0[i]);
                lenSq += res[i] * res[i];
            }
            double invLen = 1.0 / Math.sqrt(Math.max(1e-12, lenSq));
            for (int i = 0; i < 4; i++) res[i] *= invLen;
            return res;
        }
        double theta0 = Math.acos(Math.max(-1.0, Math.min(1.0, dot)));
        double theta = theta0 * t;
        double sinTheta = Math.sin(theta);
        double sinTheta0 = Math.sin(theta0);
        double s0 = Math.cos(theta) - dot * sinTheta / sinTheta0;
        double s1 = sinTheta / sinTheta0;
        double[] res = new double[4];
        for (int i = 0; i < 4; i++) {
            res[i] = s0 * q0[i] + s1 * q1Mod[i];
        }
        return res;
    }

    public static AnimationClip load(Path path) throws IOException {
        String content = Files.readString(path, StandardCharsets.UTF_8);
        return parseJson(content);
    }

    public void save(Path path) throws IOException {
        if (path.getParent() != null) {
            Files.createDirectories(path.getParent());
        }
        Files.writeString(path, toJsonString(), StandardCharsets.UTF_8);
    }

    public String toJsonString() {
        StringBuilder sb = new StringBuilder(8192);
        sb.append("{\n");
        sb.append("  \"format_version\": ").append(formatVersion).append(",\n");
        sb.append("  \"id\": \"").append(escapeJson(id)).append("\",\n");
        sb.append("  \"name\": \"").append(escapeJson(name)).append("\",\n");
        sb.append(String.format(Locale.ROOT, "  \"duration_sec\": %.4f,\n", durationSec));
        sb.append("  \"loop\": ").append(loop).append(",\n");
        sb.append(String.format(Locale.ROOT, "  \"frame_rate\": %.1f,\n", frameRate));
        sb.append("  \"tracks\": [\n");
        for (int i = 0; i < tracks.size(); i++) {
            Track t = tracks.get(i);
            sb.append("    {\n");
            sb.append("      \"bone\": \"").append(escapeJson(t.bone)).append("\",\n");
            sb.append("      \"frames\": [\n");
            for (int j = 0; j < t.frames.size(); j++) {
                Keyframe k = t.frames.get(j);
                sb.append(String.format(Locale.ROOT,
                    "        { \"time\": %.4f, \"pos\": [%.6f, %.6f, %.6f], \"rot\": [%.6f, %.6f, %.6f, %.6f] }%s\n",
                    k.time, k.pos[0], k.pos[1], k.pos[2], k.rot[0], k.rot[1], k.rot[2], k.rot[3],
                    (j + 1 < t.frames.size() ? "," : "")));
            }
            sb.append("      ]\n");
            sb.append("    }").append(i + 1 < tracks.size() ? ",\n" : "\n");
        }
        sb.append("  ]\n");
        sb.append("}\n");
        return sb.toString();
    }

    public static AnimationClip parseJson(String text) {
        AnimationClip clip = new AnimationClip();

        Matcher mv = Pattern.compile("\"format_version\"\\s*:\\s*(\\d+)").matcher(text);
        if (mv.find()) clip.formatVersion = Integer.parseInt(mv.group(1));

        Matcher mid = Pattern.compile("\"id\"\\s*:\\s*\"([^\"]*)\"").matcher(text);
        if (mid.find()) clip.id = mid.group(1);

        Matcher mn = Pattern.compile("\"name\"\\s*:\\s*\"([^\"]*)\"").matcher(text);
        if (mn.find()) clip.name = mn.group(1);

        Matcher md = Pattern.compile("\"duration_sec\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)").matcher(text);
        if (md.find()) clip.durationSec = Double.parseDouble(md.group(1));

        Matcher ml = Pattern.compile("\"loop\"\\s*:\\s*(true|false)").matcher(text);
        if (ml.find()) clip.loop = Boolean.parseBoolean(ml.group(1));

        Matcher mf = Pattern.compile("\"frame_rate\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)").matcher(text);
        if (mf.find()) clip.frameRate = Double.parseDouble(mf.group(1));

        int tracksIdx = text.indexOf("\"tracks\"");
        if (tracksIdx >= 0) {
            int openBracket = text.indexOf('[', tracksIdx);
            if (openBracket >= 0) {
                int depth = 0;
                int closeBracket = -1;
                boolean inString = false;
                for (int i = openBracket; i < text.length(); i++) {
                    char c = text.charAt(i);
                    if (c == '"' && (i == 0 || text.charAt(i - 1) != '\\')) {
                        inString = !inString;
                    } else if (!inString) {
                        if (c == '[') depth++;
                        else if (c == ']') {
                            depth--;
                            if (depth == 0) { closeBracket = i; break; }
                        }
                    }
                }
                if (closeBracket > openBracket) {
                    String tracksBody = text.substring(openBracket + 1, closeBracket);
                    List<String> trackBlobs = extractTopLevelObjects(tracksBody);
                    for (String tb : trackBlobs) {
                        Track track = parseTrackBlob(tb);
                        if (track != null) clip.tracks.add(track);
                    }
                }
            }
        }
        return clip;
    }

    private static List<String> extractTopLevelObjects(String body) {
        List<String> list = new ArrayList<>();
        int braceDepth = 0;
        int objStart = -1;
        boolean inString = false;
        for (int i = 0; i < body.length(); i++) {
            char c = body.charAt(i);
            if (c == '"' && (i == 0 || body.charAt(i - 1) != '\\')) {
                inString = !inString;
            } else if (!inString) {
                if (c == '{') {
                    if (braceDepth == 0) objStart = i;
                    braceDepth++;
                } else if (c == '}') {
                    braceDepth--;
                    if (braceDepth == 0 && objStart >= 0) {
                        list.add(body.substring(objStart, i + 1));
                        objStart = -1;
                    }
                }
            }
        }
        return list;
    }

    private static Track parseTrackBlob(String blob) {
        Matcher mb = Pattern.compile("\"bone\"\\s*:\\s*\"([^\"]*)\"").matcher(blob);
        String boneName = mb.find() ? mb.group(1) : "";
        Track track = new Track(boneName);

        int framesIdx = blob.indexOf("\"frames\"");
        if (framesIdx >= 0) {
            int open = blob.indexOf('[', framesIdx);
            if (open >= 0) {
                int depth = 0;
                int close = -1;
                boolean inString = false;
                for (int i = open; i < blob.length(); i++) {
                    char c = blob.charAt(i);
                    if (c == '"' && (i == 0 || blob.charAt(i - 1) != '\\')) {
                        inString = !inString;
                    } else if (!inString) {
                        if (c == '[') depth++;
                        else if (c == ']') {
                            depth--;
                            if (depth == 0) { close = i; break; }
                        }
                    }
                }
                if (close > open) {
                    String framesBody = blob.substring(open + 1, close);
                    List<String> frameBlobs = extractTopLevelObjects(framesBody);
                    for (String fb : frameBlobs) {
                        Keyframe kf = parseFrameBlob(fb);
                        if (kf != null) track.frames.add(kf);
                    }
                }
            }
        }
        return track;
    }

    private static Keyframe parseFrameBlob(String blob) {
        Keyframe kf = new Keyframe();
        Matcher mt = Pattern.compile("\"time\"\\s*:\\s*([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)").matcher(blob);
        if (mt.find()) kf.time = Double.parseDouble(mt.group(1));

        int posIdx = blob.indexOf("\"pos\"");
        if (posIdx >= 0) {
            int pOpen = blob.indexOf('[', posIdx);
            int pClose = blob.indexOf(']', pOpen);
            if (pOpen >= 0 && pClose > pOpen) {
                double[] p = parseDoubles(blob.substring(pOpen + 1, pClose));
                if (p.length >= 3) {
                    kf.pos[0] = p[0];
                    kf.pos[1] = p[1];
                    kf.pos[2] = p[2];
                }
            }
        }

        int rotIdx = blob.indexOf("\"rot\"");
        if (rotIdx >= 0) {
            int rOpen = blob.indexOf('[', rotIdx);
            int rClose = blob.indexOf(']', rOpen);
            if (rOpen >= 0 && rClose > rOpen) {
                double[] r = parseDoubles(blob.substring(rOpen + 1, rClose));
                if (r.length >= 4) {
                    kf.rot[0] = r[0];
                    kf.rot[1] = r[1];
                    kf.rot[2] = r[2];
                    kf.rot[3] = r[3];
                }
            }
        }
        return kf;
    }

    private static double[] parseDoubles(String sub) {
        Matcher m = Pattern.compile("[-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?").matcher(sub);
        List<Double> list = new ArrayList<>();
        while (m.find()) {
            list.add(Double.parseDouble(m.group()));
        }
        double[] arr = new double[list.size()];
        for (int i = 0; i < arr.length; i++) arr[i] = list.get(i);
        return arr;
    }

    private static String escapeJson(String s) {
        if (s == null) return "";
        return s.replace("\\", "\\\\").replace("\"", "\\\"");
    }
}
