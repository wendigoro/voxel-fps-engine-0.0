package voxel.painter.ui;

import java.awt.Color;
import java.awt.Dimension;
import java.awt.FlowLayout;
import java.awt.GridLayout;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.ButtonGroup;
import javax.swing.DefaultListCellRenderer;
import javax.swing.DefaultListModel;
import javax.swing.JButton;
import javax.swing.JCheckBox;
import javax.swing.JColorChooser;
import javax.swing.JLabel;
import javax.swing.JList;
import javax.swing.JPanel;
import javax.swing.JRadioButton;
import javax.swing.JScrollPane;
import javax.swing.JSlider;
import javax.swing.JSpinner;
import javax.swing.ListSelectionModel;
import javax.swing.SpinnerNumberModel;
import javax.swing.SwingConstants;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.PaintTools;
import voxel.painter.grid.WeaponParts;
import javax.swing.JComboBox;

public final class ToolDockPanel extends JPanel implements PainterModel.Listener {
    private final PainterModel model;
    private final JList<MaterialPalette.Entry> materialList;
    private final JSlider gradientSlider;
    private final JButton wellA;
    private final JButton wellB;
    private final JCheckBox useGradient;
    private final JLabel swatch;
    private final JSpinner sizeSpinner;
    private final JSpinner stretchSpinner;
    private boolean syncing;

    public ToolDockPanel(PainterModel model) {
        this.model = model;
        setLayout(new BoxLayout(this, BoxLayout.Y_AXIS));
        setBorder(BorderFactory.createEmptyBorder(8, 8, 8, 8));
        setPreferredSize(new Dimension(270, 640));

        add(title("Tool mode"));
        JPanel modes = new JPanel(new GridLayout(0, 2, 4, 4));
        ButtonGroup modeGroup = new ButtonGroup();
        for (PainterModel.UiTool t : PainterModel.UiTool.values()) {
            JRadioButton rb = new JRadioButton(t.name());
            rb.setSelected(t == model.uiTool());
            rb.addActionListener(e -> { if (!syncing) model.setUiTool(t); });
            rb.putClientProperty("uitool", t);
            modeGroup.add(rb);
            modes.add(rb);
        }
        modes.setAlignmentX(LEFT_ALIGNMENT);
        add(modes);

        add(title("Brush shape"));
        JPanel shapes = new JPanel(new GridLayout(0, 2, 4, 4));
        ButtonGroup shapeGroup = new ButtonGroup();
        for (PaintTools.BrushShape s : PaintTools.BrushShape.values()) {
            JRadioButton rb = new JRadioButton(s.name());
            rb.setSelected(s == model.tools().shape);
            rb.addActionListener(e -> { if (!syncing) model.setBrushShape(s); });
            rb.putClientProperty("shape", s);
            shapeGroup.add(rb);
            shapes.add(rb);
        }
        shapes.setAlignmentX(LEFT_ALIGNMENT);
        add(shapes);

        JPanel sizeRow = new JPanel(new FlowLayout(FlowLayout.LEFT));
        sizeRow.add(new JLabel("Brush size (odd)"));
        sizeSpinner = new JSpinner(new SpinnerNumberModel(model.tools().brushSize, 1, 31, 2));
        sizeSpinner.addChangeListener(e -> {
            if (!syncing) model.setBrushSize((Integer) sizeSpinner.getValue());
        });
        sizeRow.add(sizeSpinner);
        sizeRow.setAlignmentX(LEFT_ALIGNMENT);
        add(sizeRow);

        JPanel stretchRow = new JPanel(new FlowLayout(FlowLayout.LEFT));
        stretchRow.add(new JLabel("Stretch factor"));
        stretchSpinner = new JSpinner(new SpinnerNumberModel(model.stretchFactor(), 1, 8, 1));
        stretchSpinner.addChangeListener(e -> {
            if (!syncing) model.setStretchFactor((Integer) stretchSpinner.getValue());
        });
        stretchRow.add(stretchSpinner);
        stretchRow.setAlignmentX(LEFT_ALIGNMENT);
        add(stretchRow);

        JButton alt = new JButton("Toggle A/B material");
        alt.setAlignmentX(LEFT_ALIGNMENT);
        alt.addActionListener(e -> model.toggleAlt());
        add(alt);

add(Box.createVerticalStrut(8));
        add(title("Weapon part"));
        JComboBox<String> partBox = new JComboBox<>(new String[]{
                "none", "barrel", "action", "bolt_chamber", "trigger", "grip_stock", "sight"
        });
        partBox.setSelectedItem(WeaponParts.name(model.activePart()));
        partBox.setAlignmentX(LEFT_ALIGNMENT);
        partBox.addActionListener(e -> {
            if (!syncing) {
                String p = (String) partBox.getSelectedItem();
                model.setActivePart(WeaponParts.idFromName(p));
            }
        });
        add(partBox);
        JComboBox<String> calBox = new JComboBox<>(new String[]{"light", "medium", "heavy", "energy"});
        calBox.setSelectedItem(model.document().caliber == null ? "medium" : model.document().caliber);
        calBox.setAlignmentX(LEFT_ALIGNMENT);
        calBox.addActionListener(e -> {
            if (!syncing) model.setCaliber((String) calBox.getSelectedItem());
        });
        add(new JLabel("Caliber"));
        add(calBox);
        JButton bakeW = new JButton("Bake starter rifle");
        bakeW.setAlignmentX(LEFT_ALIGNMENT);
        bakeW.addActionListener(e -> model.bakeStarterWeapon());
        add(bakeW);

        add(Box.createVerticalStrut(8));
        add(title("Materials"));
        DefaultListModel<MaterialPalette.Entry> lm = new DefaultListModel<>();
        for (MaterialPalette.Entry e : MaterialPalette.entries()) {
            if (e.id != MaterialPalette.AIR) lm.addElement(e);
        }
        materialList = new JList<>(lm);
        materialList.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        materialList.setCellRenderer(new DefaultListCellRenderer() {
            @Override
            public java.awt.Component getListCellRendererComponent(
                    JList<?> list, Object value, int index, boolean isSelected, boolean cellHasFocus) {
                JLabel lab = (JLabel) super.getListCellRendererComponent(
                        list, value, index, isSelected, cellHasFocus);
                if (value instanceof MaterialPalette.Entry me) {
                    lab.setText(me.id + " " + me.name);
                    lab.setIcon(new ColorIcon(new Color(me.rgb), 14));
                }
                return lab;
            }
        });
        materialList.addListSelectionListener(e -> {
            if (!e.getValueIsAdjusting() && !syncing) {
                MaterialPalette.Entry me = materialList.getSelectedValue();
                if (me != null) model.setPrimaryMaterial(me.id);
            }
        });
        materialList.setSelectedIndex(0);
        JScrollPane scroll = new JScrollPane(materialList);
        scroll.setPreferredSize(new Dimension(250, 150));
        scroll.setAlignmentX(LEFT_ALIGNMENT);
        add(scroll);

        JButton setAlt = new JButton("Set selected as B");
        setAlt.setAlignmentX(LEFT_ALIGNMENT);
        setAlt.addActionListener(e -> {
            MaterialPalette.Entry me = materialList.getSelectedValue();
            if (me != null) model.setAltMaterial(me.id);
        });
        add(setAlt);

        add(Box.createVerticalStrut(8));
        add(title("F-gradient ramp"));
        JPanel wells = new JPanel(new FlowLayout(FlowLayout.LEFT));
        wellA = colorWell(model.gradA(), true);
        wellB = colorWell(model.gradB(), false);
        wells.add(new JLabel("A"));
        wells.add(wellA);
        wells.add(new JLabel("B"));
        wells.add(wellB);
        wells.setAlignmentX(LEFT_ALIGNMENT);
        add(wells);

        gradientSlider = new JSlider(0, 100, Math.round(model.gradientT() * 100));
        gradientSlider.addChangeListener(e -> {
            if (!syncing) {
                model.setGradientT(gradientSlider.getValue() / 100f);
                refreshSwatch();
            }
        });
        gradientSlider.setAlignmentX(LEFT_ALIGNMENT);
        add(gradientSlider);

        useGradient = new JCheckBox("Use F-gradient RGB when painting");
        useGradient.setSelected(model.useGradientRgb());
        useGradient.addActionListener(e -> model.setUseGradientRgb(useGradient.isSelected()));
        useGradient.setAlignmentX(LEFT_ALIGNMENT);
        add(useGradient);

        swatch = new JLabel(" sample ", SwingConstants.CENTER);
        swatch.setOpaque(true);
        swatch.setBorder(BorderFactory.createLineBorder(Color.DARK_GRAY));
        swatch.setAlignmentX(LEFT_ALIGNMENT);
        swatch.setMaximumSize(new Dimension(Integer.MAX_VALUE, 28));
        add(swatch);
        refreshSwatch();
        model.addListener(this);
    }

    private JLabel title(String t) {
        JLabel l = new JLabel(t);
        l.setAlignmentX(LEFT_ALIGNMENT);
        l.setBorder(BorderFactory.createEmptyBorder(4, 0, 4, 0));
        return l;
    }

    private JButton colorWell(int rgb, boolean isA) {
        JButton b = new JButton("  ");
        b.setBackground(new Color(rgb));
        b.setOpaque(true);
        b.setPreferredSize(new Dimension(36, 24));
        b.addActionListener(e -> {
            Color c = JColorChooser.showDialog(
                    this, isA ? "Gradient A" : "Gradient B", b.getBackground());
            if (c != null) {
                b.setBackground(c);
                int v = (c.getRed() << 16) | (c.getGreen() << 8) | c.getBlue();
                if (isA) model.setGradA(v);
                else model.setGradB(v);
                refreshSwatch();
            }
        });
        return b;
    }

    private void refreshSwatch() {
        PaintTools tmp = new PaintTools();
        tmp.sampleFGradient(model.gradA(), model.gradB(), model.gradientT());
        int rgb = tmp.activeRgb();
        swatch.setBackground(new Color(rgb));
        swatch.setText(String.format(" #%06X ", rgb & 0xFFFFFF));
    }

    @Override
    public void documentChanged() {}

    @Override
    public void sliceChanged() {}

    @Override
    public void toolsChanged() {
        syncing = true;
        try {
            sizeSpinner.setValue(model.tools().brushSize);
            stretchSpinner.setValue(model.stretchFactor());
            useGradient.setSelected(model.useGradientRgb());
            wellA.setBackground(new Color(model.gradA()));
            wellB.setBackground(new Color(model.gradB()));
            gradientSlider.setValue(Math.round(model.gradientT() * 100));
            refreshSwatch();
            for (java.awt.Component c : getComponents()) {
                if (c instanceof JPanel p) {
                    for (java.awt.Component cc : p.getComponents()) {
                        if (cc instanceof JRadioButton rb) {
                            Object ut = rb.getClientProperty("uitool");
                            if (ut instanceof PainterModel.UiTool t) {
                                rb.setSelected(t == model.uiTool());
                            }
                            Object sh = rb.getClientProperty("shape");
                            if (sh instanceof PaintTools.BrushShape s) {
                                rb.setSelected(s == model.tools().shape);
                            }
                        }
                    }
                }
            }
        } finally {
            syncing = false;
        }
    }

    static final class ColorIcon implements javax.swing.Icon {
        private final Color color;
        private final int size;

        ColorIcon(Color color, int size) {
            this.color = color == null ? Color.GRAY : color;
            this.size = size;
        }

        @Override
        public void paintIcon(java.awt.Component c, java.awt.Graphics g, int x, int y) {
            g.setColor(color);
            g.fillRect(x, y, size, size);
            g.setColor(Color.DARK_GRAY);
            g.drawRect(x, y, size - 1, size - 1);
        }

        @Override
        public int getIconWidth() {
            return size;
        }

        @Override
        public int getIconHeight() {
            return size;
        }
    }
}
