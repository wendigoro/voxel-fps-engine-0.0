package voxel.painter.ui;

import java.awt.BasicStroke;
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
import javax.swing.BorderFactory;
import javax.swing.JComboBox;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JSlider;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.VoxelGrid;

public final class OrthoSlicePanel extends JPanel implements PainterModel.Listener {
    private final PainterModel model;
    private final Canvas canvas;
    private final JSlider sliceSlider;
    private final JLabel sliceLabel;
    private final JComboBox<PainterModel.SliceAxis> axisBox;
    private boolean syncing;

    public OrthoSlicePanel(PainterModel model) {
        this.model = model;
        setLayout(new BorderLayout(4, 4));
        setBorder(BorderFactory.createTitledBorder("Ortho slice (unit cubes)"));

        JPanel top = new JPanel();
        axisBox = new JComboBox<>(PainterModel.SliceAxis.values());
        axisBox.setSelectedItem(model.sliceAxis());
        axisBox.addActionListener(e -> {
            if (!syncing) {
                model.setSliceAxis((PainterModel.SliceAxis) axisBox.getSelectedItem());
            }
        });
        top.add(new JLabel("Axis"));
        top.add(axisBox);
        sliceLabel = new JLabel("0");
        sliceSlider = new JSlider(0, Math.max(1, model.maxSlice()), model.sliceIndex());
        sliceSlider.addChangeListener(e -> {
            if (!syncing) {
                model.setSliceIndex(sliceSlider.getValue());
                sliceLabel.setText(Integer.toString(model.sliceIndex()));
            }
        });
        top.add(new JLabel("Index"));
        top.add(sliceSlider);
        top.add(sliceLabel);
        add(top, BorderLayout.NORTH);

        canvas = new Canvas();
        add(canvas, BorderLayout.CENTER);
        model.addListener(this);
        refreshControls();
    }

    private void refreshControls() {
        syncing = true;
        try {
            axisBox.setSelectedItem(model.sliceAxis());
            int max = Math.max(0, model.maxSlice());
            sliceSlider.setMaximum(Math.max(1, max));
            sliceSlider.setValue(Math.min(model.sliceIndex(), max));
            sliceLabel.setText(Integer.toString(model.sliceIndex()));
        } finally {
            syncing = false;
        }
        canvas.repaint();
    }

    @Override
    public void documentChanged() {
        refreshControls();
    }

    @Override
    public void toolsChanged() {
        canvas.repaint();
    }

    @Override
    public void sliceChanged() {
        refreshControls();
    }

    private final class Canvas extends JPanel {
        private boolean painting;

        Canvas() {
            setBackground(new Color(0x1E, 0x1E, 0x22));
            setPreferredSize(new Dimension(420, 420));
            addMouseListener(new MouseAdapter() {
                @Override
                public void mousePressed(MouseEvent e) {
                    painting = true;
                    apply(e);
                }

                @Override
                public void mouseReleased(MouseEvent e) {
                    painting = false;
                }
            });
            addMouseMotionListener(new MouseMotionAdapter() {
                @Override
                public void mouseDragged(MouseEvent e) {
                    if (painting) apply(e);
                }
            });
            addMouseWheelListener((MouseWheelEvent e) ->
                    model.setSliceIndex(model.sliceIndex() - e.getWheelRotation()));
        }

        private void apply(MouseEvent e) {
            int[] uv = hit(e.getX(), e.getY());
            if (uv == null) return;
            int[] w = model.sliceToWorld(uv[0], uv[1]);
            boolean erase = e.isMetaDown()
                    || e.getButton() == MouseEvent.BUTTON3
                    || (e.getModifiersEx() & MouseEvent.BUTTON3_DOWN_MASK) != 0;
            model.applyToolAt(w[0], w[1], w[2], erase);
        }

        private int[] hit(int mx, int my) {
            int pw = model.planeWidth();
            int ph = model.planeHeight();
            if (pw < 1 || ph < 1) return null;
            int side = Math.min(getWidth(), getHeight()) - 16;
            if (side < 8) return null;
            int cell = Math.max(1, side / Math.max(pw, ph));
            int gridW = cell * pw;
            int gridH = cell * ph;
            int ox = (getWidth() - gridW) / 2;
            int oy = (getHeight() - gridH) / 2;
            int u = (mx - ox) / cell;
            int vFromTop = (my - oy) / cell;
            int v = ph - 1 - vFromTop;
            if (u < 0 || v < 0 || u >= pw || v >= ph) return null;
            return new int[] {u, v};
        }

        @Override
        protected void paintComponent(Graphics g) {
            super.paintComponent(g);
            Graphics2D g2 = (Graphics2D) g.create();
            g2.setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_OFF);
            VoxelGrid grid = model.grid();
            int pw = model.planeWidth();
            int ph = model.planeHeight();
            if (pw < 1 || ph < 1) {
                g2.dispose();
                return;
            }
            int side = Math.min(getWidth(), getHeight()) - 16;
            int cell = Math.max(1, side / Math.max(pw, ph));
            int gridW = cell * pw;
            int gridH = cell * ph;
            int ox = (getWidth() - gridW) / 2;
            int oy = (getHeight() - gridH) / 2;

            for (int v = 0; v < ph; v++) {
                for (int u = 0; u < pw; u++) {
                    int[] w = model.sliceToWorld(u, v);
                    int mat = grid.getMat(w[0], w[1], w[2]);
                    int rgb = grid.getRgb(w[0], w[1], w[2]);
                    int drawY = ph - 1 - v;
                    int px = ox + u * cell;
                    int py = oy + drawY * cell;
                    if (mat == MaterialPalette.AIR && rgb == 0) {
                        boolean checker = ((u + v) & 1) == 0;
                        g2.setColor(checker ? new Color(0x2A, 0x2A, 0x30) : new Color(0x24, 0x24, 0x28));
                        g2.fillRect(px, py, cell, cell);
                    } else {
                        if (rgb == 0) rgb = MaterialPalette.defaultRgb(mat);
                        g2.setColor(new Color(rgb));
                        g2.fillRect(px, py, cell, cell);
                    }
                    if (cell >= 4) {
                        g2.setColor(new Color(0, 0, 0, 40));
                        g2.drawRect(px, py, cell, cell);
                    }
                }
            }
            g2.setColor(new Color(0x88, 0xAA, 0xFF));
            g2.setStroke(new BasicStroke(2f));
            g2.drawRect(ox, oy, gridW, gridH);
            g2.setColor(Color.LIGHT_GRAY);
            g2.drawString(
                    model.sliceAxis() + " slice " + model.sliceIndex()
                            + " unit cubes VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE,
                    8, 16);
            g2.dispose();
        }
    }
}
