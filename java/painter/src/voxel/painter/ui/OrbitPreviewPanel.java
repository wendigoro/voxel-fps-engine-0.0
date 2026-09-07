package voxel.painter.ui;

import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Dimension;
import java.awt.Graphics;
import java.awt.Graphics2D;
import java.awt.RenderingHints;
import java.awt.event.MouseAdapter;
import java.awt.event.MouseEvent;
import java.awt.event.MouseMotionAdapter;
import java.awt.event.MouseWheelEvent;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import javax.swing.BorderFactory;
import javax.swing.JPanel;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.VoxelGrid;

public final class OrbitPreviewPanel extends JPanel implements PainterModel.Listener {
    private final PainterModel model;
    private final Canvas canvas;

    public OrbitPreviewPanel(PainterModel model) {
        this.model = model;
        setLayout(new BorderLayout());
        setBorder(BorderFactory.createTitledBorder("3D orbit preview (unit cubes)"));
        canvas = new Canvas();
        add(canvas, BorderLayout.CENTER);
        model.addListener(this);
    }

    @Override
    public void documentChanged() {
        canvas.repaint();
    }

    @Override
    public void toolsChanged() {
        canvas.repaint();
    }

    @Override
    public void sliceChanged() {
        canvas.repaint();
    }

    private final class Canvas extends JPanel {
        private double yaw = 0.6;
        private double pitch = 0.45;
        private double zoom = 12.0;
        private int lastX;
        private int lastY;
        private boolean drag;

        Canvas() {
            setBackground(new Color(0x12, 0x14, 0x18));
            setPreferredSize(new Dimension(420, 420));
            addMouseListener(new MouseAdapter() {
                @Override
                public void mousePressed(MouseEvent e) {
                    drag = true;
                    lastX = e.getX();
                    lastY = e.getY();
                }

                @Override
                public void mouseReleased(MouseEvent e) {
                    drag = false;
                }
            });
            addMouseMotionListener(new MouseMotionAdapter() {
                @Override
                public void mouseDragged(MouseEvent e) {
                    if (!drag) return;
                    yaw += (e.getX() - lastX) * 0.01;
                    pitch += (e.getY() - lastY) * 0.01;
                    pitch = Math.max(-1.2, Math.min(1.2, pitch));
                    lastX = e.getX();
                    lastY = e.getY();
                    repaint();
                }
            });
            addMouseWheelListener((MouseWheelEvent e) -> {
                zoom *= (e.getWheelRotation() > 0) ? 1.1 : 0.9;
                zoom = Math.max(4.0, Math.min(80.0, zoom));
                repaint();
            });
        }

        @Override
        protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            Graphics2D g2 = (Graphics2D) g.create();
            g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON);
            VoxelGrid grid = model.grid();
            double cx = (grid.sizeX() - 1) * 0.5;
            double cy = (grid.sizeY() - 1) * 0.5;
            double cz = (grid.sizeZ() - 1) * 0.5;
            List<Proj> projs = new ArrayList<>();
            double cosY = Math.cos(yaw), sinY = Math.sin(yaw);
            double cosP = Math.cos(pitch), sinP = Math.sin(pitch);

            for (int y = 0; y < grid.sizeY(); y++) {
                for (int z = 0; z < grid.sizeZ(); z++) {
                    for (int x = 0; x < grid.sizeX(); x++) {
                        int m = grid.getMat(x, y, z);
                        int rgb = grid.getRgb(x, y, z);
                        if (m == MaterialPalette.AIR && rgb == 0) continue;
                        double X = x - cx, Y = y - cy, Z = z - cz;
                        double x1 = X * cosY - Z * sinY;
                        double z1 = X * sinY + Z * cosY;
                        double y1 = Y * cosP - z1 * sinP;
                        double z2 = Y * sinP + z1 * cosP;
                        if (rgb == 0) rgb = MaterialPalette.defaultRgb(m);
                        projs.add(new Proj(x1, y1, z2, rgb));
                    }
                }
            }
            projs.sort(Comparator.comparingDouble((Proj p) -> p.z).reversed());

            int w = getWidth(), h = getHeight();
            double scale = Math.min(w, h) / zoom;
            g2.setColor(new Color(0x2A, 0x2E, 0x36));
            for (int i = -8; i <= 8; i++) {
                double[] a = project(-8, 0, i, cosY, sinY, cosP, sinP, scale, w, h);
                double[] b = project(8, 0, i, cosY, sinY, cosP, sinP, scale, w, h);
                g2.drawLine((int) a[0], (int) a[1], (int) b[0], (int) b[1]);
                a = project(i, 0, -8, cosY, sinY, cosP, sinP, scale, w, h);
                b = project(i, 0, 8, cosY, sinY, cosP, sinP, scale, w, h);
                g2.drawLine((int) a[0], (int) a[1], (int) b[0], (int) b[1]);
            }
            for (Proj p : projs) {
                Color col = new Color(p.rgb);
                double s = scale * 0.9;
                int px = (int) (w * 0.5 + p.x * scale);
                int py = (int) (h * 0.5 - p.y * scale);
                float shade = (float) Math.max(0.35, Math.min(1.0, 0.55 + p.z * 0.03));
                Color shaded = new Color(
                        clamp(col.getRed() * shade),
                        clamp(col.getGreen() * shade),
                        clamp(col.getBlue() * shade));
                g2.setColor(shaded);
                int half = Math.max(1, (int) (s / 2));
                g2.fillRect(px - half, py - half, half * 2, half * 2);
                g2.setColor(shaded.darker());
                g2.drawRect(px - half, py - half, half * 2, half * 2);
            }
            g2.setColor(Color.LIGHT_GRAY);
            g2.drawString(
                    "drag orbit · solids=" + grid.solidCount() + " unit=" + grid.unitSizeX(),
                    8, 16);
            g2.dispose();
        }

        private double[] project(
                double x, double y, double z,
                double cosY, double sinY, double cosP, double sinP,
                double scale, int w, int h) {
            double x1 = x * cosY - z * sinY;
            double z1 = x * sinY + z * cosY;
            double y1 = y * cosP - z1 * sinP;
            return new double[] {w * 0.5 + x1 * scale, h * 0.5 - y1 * scale};
        }

        private int clamp(double v) {
            return (int) Math.max(0, Math.min(255, Math.round(v)));
        }
    }

    private static final class Proj {
        final double x, y, z;
        final int rgb;

        Proj(double x, double y, double z, int rgb) {
            this.x = x;
            this.y = y;
            this.z = z;
            this.rgb = rgb;
        }
    }
}
