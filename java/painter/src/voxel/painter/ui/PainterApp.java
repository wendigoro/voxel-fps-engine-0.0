package voxel.painter.ui;

import java.awt.BorderLayout;
import java.awt.Dimension;
import java.awt.GridLayout;
import java.awt.event.InputEvent;
import java.awt.event.KeyEvent;
import java.awt.image.BufferedImage;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Locale;
import javax.imageio.ImageIO;
import javax.swing.JFileChooser;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JMenu;
import javax.swing.JMenuBar;
import javax.swing.JMenuItem;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JSplitPane;
import javax.swing.JTabbedPane;
import javax.swing.JTextArea;
import javax.swing.KeyStroke;
import javax.swing.SwingUtilities;
import javax.swing.UIManager;
import javax.swing.WindowConstants;
import javax.swing.filechooser.FileNameExtensionFilter;
import voxel.painter.filter.BitcrushUpscale;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxIO;
import voxel.painter.grid.VoxelGrid;
import voxel.painter.grid.WeaponParts;

/** Swing painter shell using voxel.painter.grid public API. */
public final class PainterApp extends JFrame implements PainterModel.Listener {
    private final PainterModel model = new PainterModel();
    private final JLabel status = new JLabel(" ");
    private final JTextArea logArea = new JTextArea(6, 40);
    private final JTabbedPane modeTabs = new JTabbedPane();
    private final ScriptRunner scripts;
    private Path lastDir;
    private boolean syncingTabs;

    public PainterApp(Path repoRoot) {
        super("Voxel Painter — unit cubes (VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE + ")");
        this.scripts = new ScriptRunner(repoRoot, this::appendLog);
        this.lastDir = repoRoot.resolve("data");

        setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);
        setMinimumSize(new Dimension(1100, 720));
        setJMenuBar(buildMenu());

        JPanel center = new JPanel(new BorderLayout(6, 6));
modeTabs.addTab("Model", modePlaceholder(VoxDocument.Mode.MODEL));
        modeTabs.addTab("Sky tiles/background", modePlaceholder(VoxDocument.Mode.SKY));
        modeTabs.addTab("Character", modePlaceholder(VoxDocument.Mode.CHARACTER));
        modeTabs.addTab("Weapon", modePlaceholder(VoxDocument.Mode.WEAPON));
        modeTabs.addChangeListener(e -> {
            if (syncingTabs) return;
            int i = modeTabs.getSelectedIndex();
            VoxDocument.Mode mode = switch (i) {
                case 1 -> VoxDocument.Mode.SKY;
                case 2 -> VoxDocument.Mode.CHARACTER;
                case 3 -> VoxDocument.Mode.WEAPON;
                default -> VoxDocument.Mode.MODEL;
            };
            if (model.document().mode != mode) {
                int choice = JOptionPane.showConfirmDialog(
                        this,
                        "Switch mode and load defaults for " + mode.name() + "?",
                        "Mode",
                        JOptionPane.YES_NO_OPTION);
                if (choice == JOptionPane.YES_OPTION) {
                    model.newDocument(mode);
                    if (mode == VoxDocument.Mode.SKY) {
                        model.setSliceAxis(PainterModel.SliceAxis.Y);
                        model.setSliceIndex(0);
                    }
                } else {
                    syncModeTab();
                }
            }
        });

        OrthoSlicePanel ortho = new OrthoSlicePanel(model);
        OrbitPreviewPanel orbit = new OrbitPreviewPanel(model);
        JSplitPane views = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, ortho, orbit);
        views.setResizeWeight(0.5);

        JPanel main = new JPanel(new BorderLayout(4, 4));
        main.add(modeTabs, BorderLayout.NORTH);
        main.add(views, BorderLayout.CENTER);
        main.add(new ModeExtrasPanel(model), BorderLayout.SOUTH);

        ToolDockPanel tools = new ToolDockPanel(model);
        JSplitPane split = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, tools, main);
        split.setDividerLocation(280);

        logArea.setEditable(false);
        JScrollPane logScroll = new JScrollPane(logArea);
        logScroll.setBorder(javax.swing.BorderFactory.createTitledBorder("Log / script output"));

        JPanel south = new JPanel(new BorderLayout());
        south.add(status, BorderLayout.NORTH);
        south.add(logScroll, BorderLayout.CENTER);

        center.add(split, BorderLayout.CENTER);
        center.add(south, BorderLayout.SOUTH);
        setContentPane(center);

        model.newDocument(VoxDocument.Mode.MODEL);
        model.setSliceAxis(PainterModel.SliceAxis.Y);
        model.setSliceIndex(model.grid().sizeY() / 2);

        model.addListener(this);
        status.setText(model.statusLine());
        appendLog("API: VoxelGrid PaintTools VoxIO MaterialPalette SkyAndCharacter BitcrushUpscale");
        pack();
        setLocationRelativeTo(null);
    }

    private JPanel modePlaceholder(VoxDocument.Mode mode) {
        JPanel p = new JPanel(new GridLayout(1, 1));
        p.add(new JLabel(
                "<html><b>"
                        + mode.name().toLowerCase(Locale.ROOT)
                        + "</b> — cubic unit voxels, VOXEL_SIZE="
                        + VoxelGrid.VOXEL_SIZE
                        + "</html>"));
        return p;
    }

    private JMenuBar buildMenu() {
        JMenuBar bar = new JMenuBar();
        JMenu file = new JMenu("File");
        file.add(item("New", KeyStroke.getKeyStroke(KeyEvent.VK_N, InputEvent.CTRL_DOWN_MASK), this::onNew));
        file.add(item("Open .vox.json…", KeyStroke.getKeyStroke(KeyEvent.VK_O, InputEvent.CTRL_DOWN_MASK), this::onOpen));
        file.add(item("Save", KeyStroke.getKeyStroke(KeyEvent.VK_S, InputEvent.CTRL_DOWN_MASK), this::onSave));
        file.add(item("Save As…", null, this::onSaveAs));
        file.addSeparator();
file.add(item("Export 32× bitcrush preview…", null, this::onExportPreview));
        file.add(item("Export weapon.json…", null, this::onExportWeapon));
        file.addSeparator();
        file.add(item("Exit", null, () -> System.exit(0)));
        bar.add(file);

        JMenu actions = new JMenu("Actions");
        actions.add(item("Build painter (scripts/build_painter.ps1)", null,
                () -> runIfPresent("scripts/build_painter.ps1")));
        actions.add(item("Build engine (scripts/build.ps1)", null,
                () -> runIfPresent("scripts/build.ps1")));
        actions.add(item("Run smoke (scripts/demo.ps1 -SkipInteractive)", null,
                () -> runIfPresent("scripts/demo.ps1", "-SkipInteractive")));
        actions.add(item("Run painter smoke (scripts/smoke_painter.ps1)", null,
                () -> runIfPresent("scripts/smoke_painter.ps1")));
        actions.add(item("Launch dev (scripts/launch_dev.ps1)", null,
                () -> runIfPresent("scripts/launch_dev.ps1")));
        actions.add(item("Run engine (scripts/run.ps1)", null,
                () -> runIfPresent("scripts/run.ps1")));
        bar.add(actions);

        JMenu help = new JMenu("Help");
        help.add(item("About", null, () -> JOptionPane.showMessageDialog(
                this,
                "Voxel Painter UI\nunit cubes · VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE
                        + "\ngrid: VoxelGrid, PaintTools, VoxIO, VoxDocument,\n"
                        + "MaterialPalette, SkyAndCharacter, BitcrushUpscale",
                "About",
                JOptionPane.INFORMATION_MESSAGE)));
        bar.add(help);
        return bar;
    }

    private JMenuItem item(String title, KeyStroke ks, Runnable action) {
        JMenuItem mi = new JMenuItem(title);
        if (ks != null) mi.setAccelerator(ks);
        mi.addActionListener(e -> action.run());
        return mi;
    }

    private void runIfPresent(String script, String... args) {
        if (!scripts.scriptExists(script)) {
            appendLog("Missing script (hook only): " + script);
            JOptionPane.showMessageDialog(this, "Script not present yet:\n" + script,
                    "Script missing", JOptionPane.WARNING_MESSAGE);
            return;
        }
        scripts.runScript(script, args);
    }

    private void onNew() {
Object[] opts = {"model", "sky", "character", "weapon"};
        Object pick = JOptionPane.showInputDialog(
                this, "New document mode", "New", JOptionPane.QUESTION_MESSAGE,
                null, opts, model.document().modeName());
        if (pick == null) return;
        model.newDocument(VoxDocument.parseMode(pick.toString()));
        syncModeTab();
        appendLog("New " + model.document().modeName() + " document");
    }

    private void onOpen() {
        JFileChooser fc = chooser();
        if (fc.showOpenDialog(this) != JFileChooser.APPROVE_OPTION) return;
        Path path = fc.getSelectedFile().toPath();
        try {
            VoxDocument doc = VoxIO.load(path);
            model.setDocument(doc, path, true);
            lastDir = path.getParent();
            syncModeTab();
            appendLog("Opened " + path + " solids=" + doc.grid.solidCount());
        } catch (IOException ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage(), "Open failed", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void onSave() {
        if (model.filePath() == null) {
            onSaveAs();
            return;
        }
        saveTo(model.filePath());
    }

    private void onSaveAs() {
        JFileChooser fc = chooser();
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        Path path = fc.getSelectedFile().toPath();
        String name = path.getFileName().toString().toLowerCase(Locale.ROOT);
        if (!name.endsWith(".json")) {
            path = path.resolveSibling(path.getFileName().toString() + ".vox.json");
        }
        saveTo(path);
    }

    private void saveTo(Path path) {
        try {
            VoxIO.save(model.document(), path);
            model.setDocument(model.document(), path, true);
            lastDir = path.getParent();
            appendLog("Saved " + path + " solids=" + model.grid().solidCount());
        } catch (IOException ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage(), "Save failed", JOptionPane.ERROR_MESSAGE);
        }
    }

private void onExportWeapon() {
        try {
            if (model.document().mode != VoxDocument.Mode.WEAPON) {
                int c = JOptionPane.showConfirmDialog(this,
                        "Document is not weapon mode. Bake starter rifle template?",
                        "Weapon export", JOptionPane.YES_NO_OPTION);
                if (c != JOptionPane.YES_OPTION) return;
                model.bakeStarterWeapon();
            }
            WeaponParts.Stats stats = model.weaponStats();
            JFileChooser fc = new JFileChooser(lastDir == null ? null : lastDir.toFile());
            fc.setSelectedFile(new java.io.File("starter_rifle.weapon.json"));
            if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
            Path path = fc.getSelectedFile().toPath();
            if (!path.getFileName().toString().endsWith(".json"))
                path = path.resolveSibling(path.getFileName() + ".weapon.json");
            WeaponParts.exportWeaponJson(path, "starter_rifle", stats, model.document().ammoId);
            appendLog("Weapon export " + path + " dmg=" + stats.damage + " cal=" + stats.caliber
                    + " parts=" + stats.partCounts);
            lastDir = path.getParent();
        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage(), "Weapon export failed", JOptionPane.ERROR_MESSAGE);
        }
    }

    private void onExportPreview() {
        JFileChooser fc = new JFileChooser(lastDir == null ? null : lastDir.toFile());
        fc.setFileFilter(new FileNameExtensionFilter("PNG preview", "png"));
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        Path path = fc.getSelectedFile().toPath();
        if (!path.getFileName().toString().toLowerCase(Locale.ROOT).endsWith(".png")) {
            path = path.resolveSibling(path.getFileName().toString() + ".png");
        }
        try {
            int pw = model.planeWidth();
            int ph = model.planeHeight();
            int[][] src = new int[ph][pw];
            for (int v = 0; v < ph; v++) {
                for (int u = 0; u < pw; u++) {
                    int[] w = model.sliceToWorld(u, v);
                    int mat = model.grid().getMat(w[0], w[1], w[2]);
                    int rgb = model.grid().getRgb(w[0], w[1], w[2]);
                    if (rgb == 0) rgb = MaterialPalette.defaultRgb(mat);
                    if (mat == MaterialPalette.AIR && model.grid().getRgb(w[0], w[1], w[2]) == 0) {
                        rgb = 0x101018;
                    }
                    src[ph - 1 - v][u] = rgb;
                }
            }
            int[][] out = BitcrushUpscale.upscaleAndCrush(src, 32, 16);
            BufferedImage img = new BufferedImage(out[0].length, out.length, BufferedImage.TYPE_INT_RGB);
            for (int y = 0; y < out.length; y++) {
                for (int x = 0; x < out[0].length; x++) {
                    img.setRGB(x, y, out[y][x] | 0xFF000000);
                }
            }
            if (path.getParent() != null) Files.createDirectories(path.getParent());
            ImageIO.write(img, "png", path.toFile());
            appendLog("Bitcrush export " + path + " " + out[0].length + "x" + out.length);
        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage(), "Export failed", JOptionPane.ERROR_MESSAGE);
        }
    }

    private JFileChooser chooser() {
        JFileChooser fc = new JFileChooser(lastDir == null ? null : lastDir.toFile());
        fc.setFileFilter(new FileNameExtensionFilter("Voxel JSON (*.vox.json)", "json"));
        return fc;
    }

private void syncModeTab() {
        int idx = switch (model.document().mode) {
            case SKY -> 1;
            case CHARACTER -> 2;
            case WEAPON -> 3;
            default -> 0;
        };
        if (modeTabs.getSelectedIndex() != idx) {
            syncingTabs = true;
            try {
                modeTabs.setSelectedIndex(idx);
            } finally {
                syncingTabs = false;
            }
        }
    }

    private void appendLog(String line) {
        SwingUtilities.invokeLater(() -> {
            logArea.append(line + "\n");
            logArea.setCaretPosition(logArea.getDocument().getLength());
        });
    }

    @Override
    public void documentChanged() {
        status.setText(model.statusLine());
        syncModeTab();
        setTitle("Voxel Painter — " + model.document().modeName()
                + (model.isDirty() ? " *" : "")
                + " | VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE);
    }

    @Override
    public void toolsChanged() {
        status.setText(model.statusLine());
    }

    @Override
    public void sliceChanged() {
        status.setText(model.statusLine());
    }

    public static void main(String[] args) {
        Path repo = args.length > 0
                ? Path.of(args[0]).toAbsolutePath().normalize()
                : Path.of(".").toAbsolutePath().normalize();
        SwingUtilities.invokeLater(() -> {
            try {
                UIManager.setLookAndFeel(UIManager.getSystemLookAndFeelClassName());
            } catch (Exception ignored) {
            }
            new PainterApp(repo).setVisible(true);
        });
    }
}
