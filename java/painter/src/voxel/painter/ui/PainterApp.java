package voxel.painter.ui;

import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Dimension;
import java.awt.Font;
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
import javax.swing.SwingConstants;
import javax.swing.SwingUtilities;
import javax.swing.WindowConstants;
import javax.swing.border.EmptyBorder;
import javax.swing.border.MatteBorder;
import javax.swing.filechooser.FileNameExtensionFilter;
import voxel.painter.filter.BitcrushUpscale;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxIO;
import voxel.painter.grid.VoxelGrid;
import voxel.painter.grid.WeaponParts;

/**
 * Distinct Swing GUI for the Voxel Painter — separate from the Vulkan FPS engine window.
 * Dark chrome, mode-colored header, tool dock, dual viewports, weapon assembly export.
 */
public final class PainterApp extends JFrame implements PainterModel.Listener {
    private final PainterModel model = new PainterModel();
    private final JLabel status = new JLabel(" ");
    private final JLabel brandMode = new JLabel("MODEL");
    private final JLabel brandBlurb = new JLabel(" ");
    private final JPanel headerAccent = new JPanel();
    private final JTextArea logArea = new JTextArea(5, 40);
    private final JTabbedPane modeTabs = new JTabbedPane();
    private final ScriptRunner scripts;
    private Path lastDir;
    private boolean syncingTabs;

    public PainterApp(Path repoRoot) {
        super("Voxel Painter");
        Path repo = ScriptRunner.resolveRepoRoot(repoRoot);
        this.scripts = new ScriptRunner(repo, this::appendLog);
        this.lastDir = repo.resolve("data");

        setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);
        setMinimumSize(new Dimension(1280, 800));
        getContentPane().setBackground(PainterTheme.BG);
        setJMenuBar(buildMenu());

        JPanel header = new JPanel(new BorderLayout(12, 0));
        header.setBackground(PainterTheme.HEADER);
        header.setBorder(new MatteBorder(0, 0, 1, 0, PainterTheme.BORDER));
        JPanel brandBlock = new JPanel(new BorderLayout(4, 2));
        brandBlock.setOpaque(false);
        JLabel brand = new JLabel("VOXEL PAINTER");
        brand.setForeground(PainterTheme.TEXT);
        brand.setFont(PainterTheme.titleFont());
        JLabel sub = new JLabel("Cubic unit authoring  ·  VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE
                + "  ·  Java Swing (not the Vulkan engine)");
        sub.setForeground(PainterTheme.TEXT_DIM);
        sub.setFont(PainterTheme.uiFont());
        brandBlock.add(brand, BorderLayout.NORTH);
        brandBlock.add(sub, BorderLayout.SOUTH);
        header.add(brandBlock, BorderLayout.WEST);

        JPanel modeBadge = new JPanel(new BorderLayout(8, 0));
        modeBadge.setOpaque(false);
        brandMode.setOpaque(true);
        brandMode.setBackground(PainterTheme.MODE_MODEL);
        brandMode.setForeground(Color.BLACK);
        brandMode.setFont(PainterTheme.uiFont().deriveFont(Font.BOLD, 12f));
        brandMode.setBorder(new EmptyBorder(4, 10, 4, 10));
        brandMode.setHorizontalAlignment(SwingConstants.CENTER);
        brandBlurb.setForeground(PainterTheme.TEXT_DIM);
        brandBlurb.setFont(PainterTheme.uiFont());
        modeBadge.add(brandMode, BorderLayout.WEST);
        modeBadge.add(brandBlurb, BorderLayout.CENTER);
        header.add(modeBadge, BorderLayout.CENTER);

        headerAccent.setPreferredSize(new Dimension(6, 1));
        headerAccent.setBackground(PainterTheme.MODE_MODEL);
        header.add(headerAccent, BorderLayout.EAST);

        modeTabs.setBackground(PainterTheme.BG);
        modeTabs.setForeground(PainterTheme.TEXT);
        modeTabs.addTab("Model", modeCard(VoxDocument.Mode.MODEL));
        modeTabs.addTab("Sky", modeCard(VoxDocument.Mode.SKY));
        modeTabs.addTab("Character", modeCard(VoxDocument.Mode.CHARACTER));
        modeTabs.addTab("Weapon", modeCard(VoxDocument.Mode.WEAPON));
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
                        "Switch to " + PainterTheme.modeTitle(mode) + " and load defaults?\n"
                                + PainterTheme.modeBlurb(mode),
                        "Painter mode",
                        JOptionPane.YES_NO_OPTION);
                if (choice == JOptionPane.YES_OPTION) {
                    model.newDocument(mode);
                    if (mode == VoxDocument.Mode.SKY) {
                        model.setSliceAxis(PainterModel.SliceAxis.Y);
                        model.setSliceIndex(0);
                    }
                    appendLog("Mode → " + mode.name().toLowerCase(Locale.ROOT));
                } else {
                    syncModeTab();
                }
            }
        });

        OrthoSlicePanel ortho = new OrthoSlicePanel(model);
        OrbitPreviewPanel orbit = new OrbitPreviewPanel(model);
        JSplitPane views = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, ortho, orbit);
        views.setResizeWeight(0.52);
        views.setBorder(null);
        views.setBackground(PainterTheme.BG);

        JPanel main = new JPanel(new BorderLayout(6, 6));
        main.setBackground(PainterTheme.BG);
        main.setBorder(new EmptyBorder(4, 4, 4, 4));
        main.add(modeTabs, BorderLayout.NORTH);
        main.add(views, BorderLayout.CENTER);
        main.add(new ModeExtrasPanel(model), BorderLayout.SOUTH);

        ToolDockPanel tools = new ToolDockPanel(model);
        JScrollPane toolScroll = new JScrollPane(tools);
        toolScroll.setBorder(null);
        toolScroll.getViewport().setBackground(PainterTheme.BG_PANEL);
        toolScroll.setPreferredSize(new Dimension(300, 640));

        JSplitPane split = new JSplitPane(JSplitPane.HORIZONTAL_SPLIT, toolScroll, main);
        split.setDividerLocation(300);
        split.setBorder(null);
        split.setBackground(PainterTheme.BG);

        logArea.setEditable(false);
        logArea.setBackground(PainterTheme.BG_INPUT);
        logArea.setForeground(PainterTheme.TEXT_DIM);
        logArea.setCaretColor(PainterTheme.TEXT);
        logArea.setFont(PainterTheme.monoFont());
        JScrollPane logScroll = new JScrollPane(logArea);
        logScroll.setBorder(PainterTheme.cardBorder("Log / scripts"));
        logScroll.setPreferredSize(new Dimension(200, 110));

        status.setOpaque(true);
        status.setBackground(PainterTheme.STATUS_BG);
        status.setForeground(PainterTheme.TEXT_DIM);
        status.setFont(PainterTheme.monoFont());
        status.setBorder(new EmptyBorder(4, 10, 4, 10));

        JPanel south = new JPanel(new BorderLayout());
        south.setBackground(PainterTheme.BG);
        south.add(status, BorderLayout.NORTH);
        south.add(logScroll, BorderLayout.CENTER);

        JPanel root = new JPanel(new BorderLayout());
        root.setBackground(PainterTheme.BG);
        root.add(header, BorderLayout.NORTH);
        root.add(split, BorderLayout.CENTER);
        root.add(south, BorderLayout.SOUTH);
        setContentPane(root);

        model.newDocument(VoxDocument.Mode.MODEL);
        model.setSliceAxis(PainterModel.SliceAxis.Y);
        model.setSliceIndex(model.grid().sizeY() / 2);

        model.addListener(this);
        refreshChrome();
        status.setText(model.statusLine());
        appendLog("Voxel Painter UI ready — grid API + weapon assembly + bitcrush export");
        appendLog("repoRoot=" + scripts.repoRoot());
        appendLog("Dev bridge=" + scripts.repoRoot().resolve("scripts/painter_dev.cmd"));
        appendLog("Use Dev menu for build/smoke (path-contained via painter_dev.cmd)");
        pack();
        setLocationRelativeTo(null);
    }

    private JPanel modeCard(VoxDocument.Mode mode) {
        Color c = PainterTheme.modeColor(mode);
        JPanel p = new JPanel(new BorderLayout(8, 4));
        p.setBackground(PainterTheme.BG_RAISED);
        p.setBorder(new EmptyBorder(8, 12, 10, 12));
        JLabel title = new JLabel(PainterTheme.modeTitle(mode));
        title.setForeground(c);
        title.setFont(PainterTheme.uiFont().deriveFont(Font.BOLD, 13f));
        JLabel body = new JLabel("<html>" + PainterTheme.modeBlurb(mode)
                + "<br><span style='color:#9AA4B4'>unit cubes · VOXEL_SIZE="
                + VoxelGrid.VOXEL_SIZE + "</span></html>");
        body.setForeground(PainterTheme.TEXT);
        p.add(title, BorderLayout.NORTH);
        p.add(body, BorderLayout.CENTER);
        JPanel stripe = new JPanel();
        stripe.setPreferredSize(new Dimension(4, 1));
        stripe.setBackground(c);
        p.add(stripe, BorderLayout.WEST);
        return p;
    }

    private JMenuBar buildMenu() {
        JMenuBar bar = new JMenuBar();
        bar.setBackground(PainterTheme.HEADER);
        bar.setBorder(new MatteBorder(0, 0, 1, 0, PainterTheme.BORDER));

        JMenu file = new JMenu("File");
        file.add(item("New…", KeyStroke.getKeyStroke(KeyEvent.VK_N, InputEvent.CTRL_DOWN_MASK), this::onNew));
        file.add(item("Open .vox.json…", KeyStroke.getKeyStroke(KeyEvent.VK_O, InputEvent.CTRL_DOWN_MASK), this::onOpen));
        file.add(item("Save", KeyStroke.getKeyStroke(KeyEvent.VK_S, InputEvent.CTRL_DOWN_MASK), this::onSave));
        file.add(item("Save As…", null, this::onSaveAs));
        file.addSeparator();
        file.add(item("Export 32× bitcrush preview…", null, this::onExportPreview));
        file.add(item("Export weapon.json…", null, this::onExportWeapon));
        file.addSeparator();
        file.add(item("Exit", null, () -> System.exit(0)));
        bar.add(file);

        JMenu actions = new JMenu("Dev");
        actions.add(item("Build engine", null,
                () -> runDev(ScriptRunner.Action.BUILD_ENGINE)));
        actions.add(item("Build painter (javac + smoke)", null,
                () -> runDev(ScriptRunner.Action.BUILD_PAINTER)));
        actions.addSeparator();
        actions.add(item("Smoke engine", null,
                () -> runDev(ScriptRunner.Action.SMOKE_ENGINE)));
        actions.add(item("Smoke painter", null,
                () -> runDev(ScriptRunner.Action.SMOKE_PAINTER)));
        actions.add(item("Smoke all", null,
                () -> runDev(ScriptRunner.Action.SMOKE_ALL)));
        actions.addSeparator();
        actions.add(item("Run engine (interactive)", null,
                () -> runDev(ScriptRunner.Action.RUN_ENGINE)));
        actions.add(item("Show resolved paths / help", null,
                () -> runDev(ScriptRunner.Action.HELP)));
        bar.add(actions);

        JMenu help = new JMenu("Help");
        help.add(item("About Painter", null, () -> JOptionPane.showMessageDialog(
                this,
                "Voxel Painter — distinct Java Swing authoring UI\n"
                        + "Not the Vulkan FPS engine window.\n\n"
                        + "Modes: Model · Sky · Character · Weapon assembly\n"
                        + "Cubic unit grid VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE + "\n"
                        + "Weapon parts compose stats → data/weapons/*.weapon.json\n"
                        + "Bitcrush 32× is display/export only.",
                "About Voxel Painter",
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

    private void runDev(ScriptRunner.Action action) {
        if (!scripts.bridgeExists()) {
            appendLog("Missing scripts/painter_dev.cmd under " + scripts.repoRoot());
            JOptionPane.showMessageDialog(this,
                    "Dev bridge missing:\n" + scripts.repoRoot().resolve("scripts/painter_dev.cmd"),
                    "Dev scripts", JOptionPane.WARNING_MESSAGE);
            return;
        }
        if (scripts.isBusy()) {
            appendLog("Dev action already running.");
            return;
        }
        appendLog("Dev → " + action.id + "  (" + action.label + ")");
        scripts.runAction(action).whenComplete((code, err) -> SwingUtilities.invokeLater(() -> {
            if (err != null) {
                appendLog("Dev error: " + err.getMessage());
                JOptionPane.showMessageDialog(this, err.getMessage(), "Dev failed",
                        JOptionPane.ERROR_MESSAGE);
            } else if (code != null && code != 0) {
                appendLog("Dev finished with exit " + code);
            } else {
                appendLog("Dev OK: " + action.id);
            }
        }));
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
                        "Not in weapon mode. Switch and bake starter rifle template?",
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
            if (model.document().fireMode != null) stats.fireMode = model.document().fireMode;
            String id = path.getFileName().toString().replace(".weapon.json", "").replace(".json", "");
            WeaponParts.exportWeaponJson(path, id, stats, model.document().ammoId);
            appendLog("Weapon export " + path + " dmg=" + stats.damage + " cal=" + stats.caliber
                    + " fire=" + stats.fireMode + " ammo=" + model.document().ammoId
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
        refreshChrome();
    }

    private void refreshChrome() {
        VoxDocument.Mode mode = model.document().mode;
        Color c = PainterTheme.modeColor(mode);
        brandMode.setText(PainterTheme.modeTitle(mode).toUpperCase(Locale.ROOT));
        brandMode.setBackground(c);
        brandBlurb.setText(PainterTheme.modeBlurb(mode));
        headerAccent.setBackground(c);
        setTitle("Voxel Painter — " + PainterTheme.modeTitle(mode)
                + (model.isDirty() ? " *" : "")
                + "  |  unit cubes  VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE);
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
            PainterTheme.installLookAndFeel();
            new PainterApp(repo).setVisible(true);
        });
    }
}
