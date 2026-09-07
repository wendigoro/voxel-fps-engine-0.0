package voxel.painter.ui;

import voxel.painter.filter.BitcrushUpscale;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.PaintTools;
import voxel.painter.grid.SkyAndCharacter;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxIO;
import voxel.painter.grid.VoxelGrid;

import javax.swing.BorderFactory;
import javax.swing.ButtonGroup;
import javax.swing.JButton;
import javax.swing.JComboBox;
import javax.swing.JFileChooser;
import javax.swing.JFrame;
import javax.swing.JLabel;
import javax.swing.JMenu;
import javax.swing.JMenuBar;
import javax.swing.JMenuItem;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JRadioButton;
import javax.swing.JScrollPane;
import javax.swing.JSlider;
import javax.swing.JTabbedPane;
import javax.swing.SwingUtilities;
import javax.swing.WindowConstants;
import javax.swing.filechooser.FileNameExtensionFilter;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Dimension;
import java.awt.FlowLayout;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.image.BufferedImage;
import java.io.File;
import java.nio.file.Path;
import java.util.List;

/** Basic Java UI for voxel painter tools, save, and smoke hooks. */
public final class PainterApp extends JFrame {
    private VoxDocument doc = new VoxDocument(VoxDocument.Mode.MODEL, 32, 24, 32);
    private final PaintTools tools = new PaintTools();
    private int sliceY = 0;
    private final SlicePanel slicePanel = new SlicePanel();
    private final PreviewPanel previewPanel = new PreviewPanel();
    private final JLabel status = new JLabel("Ready");
    private int gradA = 0x2244AA;
    private int gradB = 0xFFCC88;
    private final JSlider fGrad = new JSlider(0, 100, 50);
    private Path projectRoot = Path.of(".").toAbsolutePath().normalize();

    public PainterApp() {
        super("Voxel Painter — cubic unit grid");
        setDefaultCloseOperation(WindowConstants.EXIT_ON_CLOSE);
        setMinimumSize(new Dimension(1100, 720));
        setJMenuBar(buildMenu());
        add(buildToolbar(), BorderLayout.NORTH);
        JTabbedPane tabs = new JTabbedPane();
        tabs.addTab("Model", buildWorkspace());
        tabs.addTab("Sky", buildSkyTab());
        tabs.addTab("Character", buildCharacterTab());
        tabs.addChangeListener(e -> {
            int i = tabs.getSelectedIndex();
            if (i == 0) ensureMode(VoxDocument.Mode.MODEL, 32, 24, 32);
            else if (i == 1) ensureMode(VoxDocument.Mode.SKY, 28, 1, 14);
            else ensureMode(VoxDocument.Mode.CHARACTER, 24, 16, 12);
            refresh();
        });
        add(tabs, BorderLayout.CENTER);
        add(status, BorderLayout.SOUTH);
        pack();
        setLocationRelativeTo(null);
    }

    private void ensureMode(VoxDocument.Mode mode, int sx, int sy, int sz) {
        if (doc.mode == mode) return;
        doc = new VoxDocument(mode, sx, sy, sz);
        sliceY = 0;
        if (mode == VoxDocument.Mode.SKY) {
            SkyAndCharacter.paintSkyTiles(doc.grid, doc.segU, doc.segV,
                    doc.moonDirX, doc.moonDirY, doc.moonDirZ, doc.moonIntensity);
        } else if (mode == VoxDocument.Mode.CHARACTER) {
            doc.feetX = 8; doc.feetY = 0; doc.feetZ = 4;
            SkyAndCharacter.paintCharacter(doc.grid, doc.feetX, doc.feetY, doc.feetZ);
        }
    }

    private JMenuBar buildMenu() {
        JMenuBar bar = new JMenuBar();
        JMenu file = new JMenu("File");
        JMenuItem neu = new JMenuItem("New Model");
        neu.addActionListener(e -> {
            doc = new VoxDocument(VoxDocument.Mode.MODEL, 32, 24, 32);
            sliceY = 0; refresh();
        });
        JMenuItem open = new JMenuItem("Open…");
        open.addActionListener(e -> openFile());
        JMenuItem save = new JMenuItem("Save…");
        save.addActionListener(e -> saveFile());
        JMenuItem exp = new JMenuItem("Export 32× Bitcrush Preview…");
        exp.addActionListener(e -> exportPreview());
        file.add(neu); file.add(open); file.add(save); file.add(exp);
        JMenu run = new JMenu("Run");
        JMenuItem smokeP = new JMenuItem("Smoke Painter");
        smokeP.addActionListener(e -> runPs("scripts\\build_painter.ps1"));
        JMenuItem smokeE = new JMenuItem("Smoke Engine");
        smokeE.addActionListener(e -> runPs("scripts\\demo.ps1", "-SkipInteractive"));
        JMenuItem build = new JMenuItem("Build Engine");
        build.addActionListener(e -> runPs("scripts\\build.ps1"));
        run.add(smokeP); run.add(smokeE); run.add(build);
        bar.add(file); bar.add(run);
        return bar;
    }

    private JPanel buildToolbar() {
        JPanel p = new JPanel(new FlowLayout(FlowLayout.LEFT));
        JComboBox<String> mats = new JComboBox<>();
        for (MaterialPalette.Entry e : MaterialPalette.entries()) mats.addItem(e.name);
        mats.setSelectedItem("concrete");
        mats.addActionListener(e -> {
            String n = (String) mats.getSelectedItem();
            int id = MaterialPalette.idFromName(n);
            if (tools.useB) { tools.matB = id; tools.rgbB = MaterialPalette.defaultRgb(id); }
            else { tools.matA = id; tools.rgbA = MaterialPalette.defaultRgb(id); }
            status.setText("Material " + n + (tools.useB ? " (B)" : " (A)"));
        });
        JComboBox<PaintTools.BrushShape> shapes = new JComboBox<>(PaintTools.BrushShape.values());
        shapes.setSelectedItem(PaintTools.BrushShape.CUBE);
        shapes.addActionListener(e -> tools.shape = (PaintTools.BrushShape) shapes.getSelectedItem());
        JComboBox<Integer> sizes = new JComboBox<>(new Integer[]{1, 3, 5});
        sizes.addActionListener(e -> tools.brushSize = (Integer) sizes.getSelectedItem());
        JButton alt = new JButton("Alt A/B");
        alt.addActionListener(e -> { tools.toggleAlternate(); status.setText(tools.useB ? "Brush B" : "Brush A"); });
        JButton fill = new JButton("Fill");
        fill.addActionListener(e -> status.setText("Fill: click slice"));
        // fill uses next click via flag
        slicePanel.fillNext = false;
        fill.addActionListener(e -> { slicePanel.fillNext = true; status.setText("Fill armed — click voxel"); });
        JButton stretch = new JButton("Stretch 2x sel");
        stretch.addActionListener(e -> {
            PaintTools.stretch(doc.grid, 4, Math.max(0, sliceY), 4, 8, Math.max(0, sliceY), 8, 2, 1, 2);
            refresh();
            status.setText("Stretch applied (unit cubes)");
        });
        JButton drop = new JButton("Dropper");
        drop.addActionListener(e -> { slicePanel.dropperNext = true; status.setText("Dropper armed"); });
        fGrad.addChangeListener(e -> {
            tools.sampleFGradient(gradA, gradB, fGrad.getValue() / 100f);
            status.setText(String.format("F-grad t=%.2f rgb=%06X", fGrad.getValue() / 100f, tools.activeRgb()));
        });
        JButton ga = new JButton("GradA");
        ga.addActionListener(e -> pickGrad(true));
        JButton gb = new JButton("GradB");
        gb.addActionListener(e -> pickGrad(false));
        p.add(new JLabel("Mat")); p.add(mats);
        p.add(new JLabel("Brush")); p.add(shapes); p.add(sizes);
        p.add(alt); p.add(fill); p.add(stretch); p.add(drop);
        p.add(ga); p.add(gb); p.add(new JLabel("F")); p.add(fGrad);
        JSlider ySlide = new JSlider(0, Math.max(0, doc.grid.sizeY() - 1), 0);
        ySlide.addChangeListener(e -> {
            sliceY = ySlide.getValue();
            slicePanel.repaint();
            status.setText("Slice Y=" + sliceY);
        });
        p.add(new JLabel("Y")); p.add(ySlide);
        return p;
    }

    private void pickGrad(boolean a) {
        String s = JOptionPane.showInputDialog(this, "RGB hex RRGGBB", a ? String.format("%06X", gradA) : String.format("%06X", gradB));
        if (s == null) return;
        try {
            int v = Integer.parseInt(s.trim(), 16) & 0xFFFFFF;
            if (a) gradA = v; else gradB = v;
            tools.sampleFGradient(gradA, gradB, fGrad.getValue() / 100f);
        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, "Bad hex");
        }
    }

    private JPanel buildWorkspace() {
        JPanel p = new JPanel(new BorderLayout());
        p.add(new JScrollPane(slicePanel), BorderLayout.CENTER);
        p.add(previewPanel, BorderLayout.EAST);
        return p;
    }

    private JPanel buildSkyTab() {
        JPanel p = new JPanel(new BorderLayout());
        JButton bake = new JButton("Bake sky tiles + moon");
        bake.addActionListener(e -> {
            ensureMode(VoxDocument.Mode.SKY, 28, 1, 14);
            SkyAndCharacter.paintSkyTiles(doc.grid, doc.segU, doc.segV,
                    doc.moonDirX, doc.moonDirY, doc.moonDirZ, doc.moonIntensity);
            refresh();
            status.setText("Sky tiles baked (unit sheet)");
        });
        p.add(bake, BorderLayout.NORTH);
        p.add(new JScrollPane(slicePanel), BorderLayout.CENTER);
        return p;
    }

    private JPanel buildCharacterTab() {
        JPanel p = new JPanel(new BorderLayout());
        JButton place = new JButton("Place unit character");
        place.addActionListener(e -> {
            ensureMode(VoxDocument.Mode.CHARACTER, 24, 16, 12);
            doc.grid.clear();
            SkyAndCharacter.paintCharacter(doc.grid, doc.feetX, doc.feetY, doc.feetZ);
            refresh();
            status.setText("Character placed (unit voxels)");
        });
        p.add(place, BorderLayout.NORTH);
        p.add(new JScrollPane(slicePanel), BorderLayout.CENTER);
        return p;
    }

    private void refresh() {
        doc.grid.assertCubicUnitInvariant();
        slicePanel.repaint();
        previewPanel.repaint();
        status.setText(doc.modeName() + " solids=" + doc.grid.solidCount()
                + " unit=" + doc.grid.unitSizeX() + "=" + doc.grid.unitSizeY() + "=" + doc.grid.unitSizeZ());
    }

    private void openFile() {
        JFileChooser fc = new JFileChooser(projectRoot.resolve("data/voxfmt").toFile());
        fc.setFileFilter(new FileNameExtensionFilter("vox.json", "json"));
        if (fc.showOpenDialog(this) != JFileChooser.APPROVE_OPTION) return;
        try {
            doc = VoxIO.load(fc.getSelectedFile().toPath());
            sliceY = 0;
            refresh();
        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage());
        }
    }

    private void saveFile() {
        JFileChooser fc = new JFileChooser(projectRoot.resolve("data/voxfmt").toFile());
        fc.setFileFilter(new FileNameExtensionFilter("vox.json", "json"));
        if (fc.showSaveDialog(this) != JFileChooser.APPROVE_OPTION) return;
        try {
            Path p = fc.getSelectedFile().toPath();
            if (!p.getFileName().toString().endsWith(".json")) p = p.resolveSibling(p.getFileName() + ".vox.json");
            VoxIO.save(doc, p);
            status.setText("Saved " + p);
        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage());
        }
    }

    private void exportPreview() {
        try {
            int h = doc.grid.sizeZ();
            int w = doc.grid.sizeX();
            int y = Math.min(sliceY, doc.grid.sizeY() - 1);
            int[][] src = new int[h][w];
            for (int z = 0; z < h; z++)
                for (int x = 0; x < w; x++) {
                    int m = doc.grid.getMat(x, y, z);
                    int rgb = doc.grid.getRgb(x, y, z);
                    if (rgb == 0) rgb = MaterialPalette.defaultRgb(m);
                    if (m == MaterialPalette.AIR && rgb == 0) rgb = 0x101018;
                    src[z][x] = rgb;
                }
            int[][] out = BitcrushUpscale.upscaleAndCrush(src, 32, 16);
            BufferedImage img = new BufferedImage(out[0].length, out.length, BufferedImage.TYPE_INT_RGB);
            for (int yy = 0; yy < out.length; yy++)
                for (int xx = 0; xx < out[0].length; xx++)
                    img.setRGB(xx, yy, out[yy][xx] | 0xFF000000);
            File f = projectRoot.resolve("build/painter/preview_32x.png").toFile();
            f.getParentFile().mkdirs();
            javax.imageio.ImageIO.write(img, "png", f);
            status.setText("Exported " + f.getAbsolutePath());
        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage());
        }
    }

    private void runPs(String rel, String... extra) {
        try {
            Path script = projectRoot.resolve(rel);
            if (!script.toFile().exists()) {
                // try parent of cwd
                Path alt = Path.of(System.getProperty("user.dir")).resolve(rel);
                if (alt.toFile().exists()) script = alt;
            }
            java.util.ArrayList<String> cmd = new java.util.ArrayList<>();
            cmd.add("powershell");
            cmd.add("-NoProfile");
            cmd.add("-ExecutionPolicy");
            cmd.add("Bypass");
            cmd.add("-File");
            cmd.add(script.toString());
            for (String e : extra) cmd.add(e);
            Process p = new ProcessBuilder(cmd)
                    .directory(projectRoot.toFile())
                    .inheritIO()
                    .start();
            status.setText("Launched " + rel + " pid=" + p.pid());
        } catch (Exception ex) {
            JOptionPane.showMessageDialog(this, ex.getMessage());
        }
    }

    private final class SlicePanel extends JPanel {
        boolean fillNext;
        boolean dropperNext;
        private static final int CELL = 14;

        SlicePanel() {
            setPreferredSize(new Dimension(32 * CELL + 20, 32 * CELL + 20));
            setBackground(new Color(18, 18, 24));
            addMouseListener(new MouseAdapter() {
                @Override public void mousePressed(MouseEvent e) {
                    int x = e.getX() / CELL;
                    int z = e.getY() / CELL;
                    int y = sliceY;
                    if (!doc.grid.inBounds(x, y, z)) return;
                    if (dropperNext) {
                        tools.dropper(doc.grid, x, y, z);
                        dropperNext = false;
                        status.setText("Dropped mat=" + MaterialPalette.nameFromId(tools.activeMat()));
                        return;
                    }
                    if (fillNext) {
                        tools.floodFill(doc.grid, x, y, z);
                        fillNext = false;
                    } else {
                        tools.paintAt(doc.grid, x, y, z);
                    }
                    refresh();
                }
            });
        }

        @Override protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            Graphics2D g2 = (Graphics2D) g;
            VoxelGrid grid = doc.grid;
            int y = Math.min(sliceY, grid.sizeY() - 1);
            for (int z = 0; z < grid.sizeZ(); z++) {
                for (int x = 0; x < grid.sizeX(); x++) {
                    int m = grid.getMat(x, y, z);
                    int rgb = grid.getRgb(x, y, z);
                    if (m == MaterialPalette.AIR && rgb == 0) {
                        g2.setColor(new Color(30, 30, 40));
                    } else {
                        if (rgb == 0) rgb = MaterialPalette.defaultRgb(m);
                        g2.setColor(new Color(rgb));
                    }
                    g2.fillRect(x * CELL, z * CELL, CELL - 1, CELL - 1);
                }
            }
            g2.setColor(Color.GRAY);
            g2.drawString("Y=" + y + " unit cube cells", 8, getHeight() - 8);
        }
    }

    private final class PreviewPanel extends JPanel {
        PreviewPanel() {
            setPreferredSize(new Dimension(280, 280));
            setBorder(BorderFactory.createTitledBorder("Orbit preview"));
            setBackground(new Color(12, 12, 16));
        }

        @Override protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            Graphics2D g2 = (Graphics2D) g;
            double yaw = 0.7, pitch = 0.4;
            double cx = doc.grid.sizeX() / 2.0, cy = doc.grid.sizeY() / 2.0, cz = doc.grid.sizeZ() / 2.0;
            int ox = getWidth() / 2, oy = getHeight() / 2;
            double scale = 4.5;
            // draw solids as projected unit cubes (simple points/rects)
            for (int y = 0; y < doc.grid.sizeY(); y++)
                for (int z = 0; z < doc.grid.sizeZ(); z++)
                    for (int x = 0; x < doc.grid.sizeX(); x++) {
                        int m = doc.grid.getMat(x, y, z);
                        if (m == MaterialPalette.AIR && doc.grid.getRgb(x, y, z) == 0) continue;
                        double X = x - cx, Y = y - cy, Z = z - cz;
                        double x1 = X * Math.cos(yaw) - Z * Math.sin(yaw);
                        double z1 = X * Math.sin(yaw) + Z * Math.cos(yaw);
                        double y1 = Y * Math.cos(pitch) - z1 * Math.sin(pitch);
                        int px = ox + (int) (x1 * scale);
                        int py = oy - (int) (y1 * scale);
                        int rgb = doc.grid.getRgb(x, y, z);
                        if (rgb == 0) rgb = MaterialPalette.defaultRgb(m);
                        g2.setColor(new Color(rgb));
                        g2.fillRect(px, py, 3, 3);
                    }
        }
    }

    public static void main(String[] args) {
        Path root = Path.of(args.length > 0 ? args[0] : ".").toAbsolutePath().normalize();
        SwingUtilities.invokeLater(() -> {
            PainterApp app = new PainterApp();
            app.projectRoot = root;
            app.setVisible(true);
        });
    }
}
