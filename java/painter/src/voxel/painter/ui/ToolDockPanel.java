package voxel.painter.ui;

import java.awt.Color;
import java.awt.Dimension;
import java.awt.FlowLayout;
import java.awt.Font;
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
import javax.swing.JComboBox;
import javax.swing.JLabel;
import javax.swing.JList;
import javax.swing.JPanel;
import javax.swing.JRadioButton;
import javax.swing.JScrollPane;
import javax.swing.JSlider;
import javax.swing.JSpinner;
import javax.swing.JTextArea;
import javax.swing.ListSelectionModel;
import javax.swing.SpinnerNumberModel;
import javax.swing.SwingConstants;
import javax.swing.border.EmptyBorder;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.PaintTools;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.WeaponParts;

/** Left tool dock — mode-aware; weapon assembly card only in weapon mode. */
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
    private final JPanel weaponCard;
    private final JComboBox<String> partBox;
    private final JComboBox<String> calBox;
    private final JComboBox<String> ammoBox;
    private final JComboBox<String> fireBox;
    private final JTextArea statsArea;
    private boolean syncing;

    public ToolDockPanel(PainterModel model) {
        this.model = model;
        setLayout(new BoxLayout(this, BoxLayout.Y_AXIS));
        setBackground(PainterTheme.BG_PANEL);
        setBorder(new EmptyBorder(10, 10, 10, 10));
        setPreferredSize(new Dimension(280, 720));

        JLabel dockTitle = new JLabel("TOOLS");
        dockTitle.setForeground(PainterTheme.ACCENT);
        dockTitle.setFont(PainterTheme.uiFont().deriveFont(Font.BOLD, 11f));
        dockTitle.setAlignmentX(LEFT_ALIGNMENT);
        add(dockTitle);
        add(Box.createVerticalStrut(6));

        add(section("Tool mode"));
        JPanel modes = new JPanel(new GridLayout(0, 2, 4, 4));
        modes.setOpaque(false);
        ButtonGroup modeGroup = new ButtonGroup();
        for (PainterModel.UiTool t : PainterModel.UiTool.values()) {
            JRadioButton rb = new JRadioButton(t.name());
            rb.setOpaque(false);
            rb.setForeground(PainterTheme.TEXT);
            rb.setSelected(t == model.uiTool());
            rb.addActionListener(e -> { if (!syncing) model.setUiTool(t); });
            rb.putClientProperty("uitool", t);
            modeGroup.add(rb);
            modes.add(rb);
        }
        modes.setAlignmentX(LEFT_ALIGNMENT);
        add(modes);

        add(section("Brush shape"));
        JPanel shapes = new JPanel(new GridLayout(0, 2, 4, 4));
        shapes.setOpaque(false);
        ButtonGroup shapeGroup = new ButtonGroup();
        for (PaintTools.BrushShape s : PaintTools.BrushShape.values()) {
            JRadioButton rb = new JRadioButton(s.name());
            rb.setOpaque(false);
            rb.setForeground(PainterTheme.TEXT);
            rb.setSelected(s == model.tools().shape);
            rb.addActionListener(e -> { if (!syncing) model.setBrushShape(s); });
            rb.putClientProperty("shape", s);
            shapeGroup.add(rb);
            shapes.add(rb);
        }
        shapes.setAlignmentX(LEFT_ALIGNMENT);
        add(shapes);

        JPanel sizeRow = row();
        sizeRow.add(label("Brush size"));
        sizeSpinner = new JSpinner(new SpinnerNumberModel(model.tools().brushSize, 1, 31, 2));
        sizeSpinner.addChangeListener(e -> {
            if (!syncing) model.setBrushSize((Integer) sizeSpinner.getValue());
        });
        sizeRow.add(sizeSpinner);
        add(sizeRow);

        JPanel stretchRow = row();
        stretchRow.add(label("Stretch x"));
        stretchSpinner = new JSpinner(new SpinnerNumberModel(model.stretchFactor(), 1, 8, 1));
        stretchSpinner.addChangeListener(e -> {
            if (!syncing) model.setStretchFactor((Integer) stretchSpinner.getValue());
        });
        stretchRow.add(stretchSpinner);
        add(stretchRow);

        JButton alt = accentButton("Toggle A/B material");
        alt.addActionListener(e -> model.toggleAlt());
        add(alt);

        weaponCard = new JPanel();
        weaponCard.setLayout(new BoxLayout(weaponCard, BoxLayout.Y_AXIS));
        weaponCard.setBackground(PainterTheme.BG_RAISED);
        weaponCard.setBorder(PainterTheme.cardBorder("Weapon assembly", PainterTheme.MODE_WEAPON));
        weaponCard.setAlignmentX(LEFT_ALIGNMENT);

        partBox = new JComboBox<>(new String[]{
                "none", "barrel", "action", "bolt_chamber", "trigger", "grip_stock", "sight"
        });
        partBox.setSelectedItem(WeaponParts.name(model.activePart()));
        partBox.setMaximumSize(new Dimension(Integer.MAX_VALUE, 28));
        partBox.setAlignmentX(LEFT_ALIGNMENT);
        partBox.addActionListener(e -> {
            if (!syncing) {
                String p = (String) partBox.getSelectedItem();
                model.setActivePart(WeaponParts.idFromName(p));
            }
        });
        weaponCard.add(smallLabel("Paint part channel"));
        weaponCard.add(partBox);

        calBox = new JComboBox<>(new String[]{"light", "medium", "heavy", "energy"});
        calBox.setSelectedItem(model.document().caliber == null ? "medium" : model.document().caliber);
        calBox.setMaximumSize(new Dimension(Integer.MAX_VALUE, 28));
        calBox.setAlignmentX(LEFT_ALIGNMENT);
        calBox.addActionListener(e -> {
            if (!syncing) model.setCaliber((String) calBox.getSelectedItem());
        });
        weaponCard.add(smallLabel("Caliber"));
        weaponCard.add(calBox);

        ammoBox = new JComboBox<>(new String[]{
                "light_fmj", "light_hp", "light_ap",
                "medium_fmj", "medium_tracer", "medium_shred",
                "heavy_fmj", "heavy_he",
                "energy_bolt", "energy_pierce"
        });
        ammoBox.setSelectedItem(model.document().ammoId == null ? "medium_fmj" : model.document().ammoId);
        ammoBox.setMaximumSize(new Dimension(Integer.MAX_VALUE, 28));
        ammoBox.setAlignmentX(LEFT_ALIGNMENT);
        ammoBox.addActionListener(e -> {
            if (!syncing) model.setAmmoId((String) ammoBox.getSelectedItem());
        });
        weaponCard.add(smallLabel("Ammo id"));
        weaponCard.add(ammoBox);

        fireBox = new JComboBox<>(new String[]{"semi", "auto", "bolt"});
        fireBox.setSelectedItem(model.document().fireMode == null ? "semi" : model.document().fireMode);
        fireBox.setMaximumSize(new Dimension(Integer.MAX_VALUE, 28));
        fireBox.setAlignmentX(LEFT_ALIGNMENT);
        fireBox.addActionListener(e -> {
            if (!syncing) model.setFireMode((String) fireBox.getSelectedItem());
        });
        weaponCard.add(smallLabel("Fire mode"));
        weaponCard.add(fireBox);

        JButton bakeW = accentButton("Bake starter rifle");
        bakeW.setAlignmentX(LEFT_ALIGNMENT);
        bakeW.addActionListener(e -> model.bakeStarterWeapon());
        weaponCard.add(Box.createVerticalStrut(4));
        weaponCard.add(bakeW);

        statsArea = new JTextArea(6, 18);
        statsArea.setEditable(false);
        statsArea.setFont(PainterTheme.monoFont());
        statsArea.setBackground(PainterTheme.BG_INPUT);
        statsArea.setForeground(PainterTheme.TEXT);
        statsArea.setBorder(BorderFactory.createEmptyBorder(4, 4, 4, 4));
        statsArea.setAlignmentX(LEFT_ALIGNMENT);
        JScrollPane statsScroll = new JScrollPane(statsArea);
        statsScroll.setAlignmentX(LEFT_ALIGNMENT);
        statsScroll.setPreferredSize(new Dimension(240, 110));
        statsScroll.setMaximumSize(new Dimension(Integer.MAX_VALUE, 120));
        weaponCard.add(Box.createVerticalStrut(4));
        weaponCard.add(smallLabel("Composed stats"));
        weaponCard.add(statsScroll);

        add(Box.createVerticalStrut(8));
        add(weaponCard);

        add(Box.createVerticalStrut(8));
        add(section("Materials"));
        DefaultListModel<MaterialPalette.Entry> lm = new DefaultListModel<>();
        for (MaterialPalette.Entry e : MaterialPalette.entries()) {
            if (e.id != MaterialPalette.AIR) lm.addElement(e);
        }
        materialList = new JList<>(lm);
        materialList.setSelectionMode(ListSelectionModel.SINGLE_SELECTION);
        materialList.setBackground(PainterTheme.BG_INPUT);
        materialList.setForeground(PainterTheme.TEXT);
        materialList.setCellRenderer(new DefaultListCellRenderer() {
            @Override
            public java.awt.Component getListCellRendererComponent(
                    JList<?> list, Object value, int index, boolean isSelected, boolean cellHasFocus) {
                JLabel lab = (JLabel) super.getListCellRendererComponent(
                        list, value, index, isSelected, cellHasFocus);
                lab.setOpaque(true);
                lab.setBackground(isSelected ? PainterTheme.ACCENT.darker() : PainterTheme.BG_INPUT);
                lab.setForeground(PainterTheme.TEXT);
                if (value instanceof MaterialPalette.Entry me) {
                    lab.setText(me.id + "  " + me.name);
                    lab.setIcon(new ColorIcon(new Color(me.rgb), 14));
                }
                return lab;
            }
        });
        materialList.setSelectedIndex(0);
        materialList.addListSelectionListener(e -> {
            if (!e.getValueIsAdjusting() && !syncing) {
                MaterialPalette.Entry me = materialList.getSelectedValue();
                if (me != null) model.setPrimaryMaterial(me.id);
            }
        });
        JScrollPane scroll = new JScrollPane(materialList);
        scroll.setPreferredSize(new Dimension(250, 140));
        scroll.setAlignmentX(LEFT_ALIGNMENT);
        add(scroll);

        JButton setAlt = accentButton("Set selected as B");
        setAlt.addActionListener(e -> {
            MaterialPalette.Entry me = materialList.getSelectedValue();
            if (me != null) model.setAltMaterial(me.id);
        });
        add(setAlt);

        add(Box.createVerticalStrut(8));
        add(section("F-gradient ramp"));
        JPanel wells = row();
        wellA = colorWell(model.gradA(), true);
        wellB = colorWell(model.gradB(), false);
        wells.add(label("A"));
        wells.add(wellA);
        wells.add(label("B"));
        wells.add(wellB);
        add(wells);

        gradientSlider = new JSlider(0, 100, Math.round(model.gradientT() * 100));
        gradientSlider.setOpaque(false);
        gradientSlider.addChangeListener(e -> {
            if (!syncing) {
                model.setGradientT(gradientSlider.getValue() / 100f);
                refreshSwatch();
            }
        });
        gradientSlider.setAlignmentX(LEFT_ALIGNMENT);
        add(gradientSlider);

        useGradient = new JCheckBox("Use F-gradient RGB when painting");
        useGradient.setOpaque(false);
        useGradient.setForeground(PainterTheme.TEXT);
        useGradient.setSelected(model.useGradientRgb());
        useGradient.addActionListener(e -> model.setUseGradientRgb(useGradient.isSelected()));
        useGradient.setAlignmentX(LEFT_ALIGNMENT);
        add(useGradient);

        swatch = new JLabel(" sample ", SwingConstants.CENTER);
        swatch.setOpaque(true);
        swatch.setForeground(PainterTheme.TEXT);
        swatch.setBorder(BorderFactory.createLineBorder(PainterTheme.BORDER));
        swatch.setAlignmentX(LEFT_ALIGNMENT);
        swatch.setMaximumSize(new Dimension(Integer.MAX_VALUE, 28));
        add(swatch);
        refreshSwatch();
        refreshWeaponVisibility();
        refreshWeaponStats();
        model.addListener(this);
    }

    private JLabel section(String t) {
        JLabel l = new JLabel(t.toUpperCase());
        l.setAlignmentX(LEFT_ALIGNMENT);
        l.setForeground(PainterTheme.TEXT_DIM);
        l.setFont(PainterTheme.uiFont().deriveFont(Font.BOLD, 10f));
        l.setBorder(new EmptyBorder(8, 0, 4, 0));
        return l;
    }

    private JLabel label(String t) {
        JLabel l = new JLabel(t);
        l.setForeground(PainterTheme.TEXT);
        return l;
    }

    private JLabel smallLabel(String t) {
        JLabel l = new JLabel(t);
        l.setForeground(PainterTheme.TEXT_DIM);
        l.setFont(PainterTheme.uiFont().deriveFont(10f));
        l.setAlignmentX(LEFT_ALIGNMENT);
        l.setBorder(new EmptyBorder(4, 0, 2, 0));
        return l;
    }

    private JPanel row() {
        JPanel p = new JPanel(new FlowLayout(FlowLayout.LEFT, 4, 2));
        p.setOpaque(false);
        p.setAlignmentX(LEFT_ALIGNMENT);
        return p;
    }

    private JButton accentButton(String text) {
        JButton b = new JButton(text);
        b.setAlignmentX(LEFT_ALIGNMENT);
        b.setBackground(PainterTheme.BG_RAISED);
        b.setForeground(PainterTheme.TEXT);
        b.setFocusPainted(false);
        b.setMaximumSize(new Dimension(Integer.MAX_VALUE, 28));
        return b;
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

    private void refreshWeaponVisibility() {
        boolean w = model.document().mode == VoxDocument.Mode.WEAPON;
        weaponCard.setVisible(w);
        revalidate();
        repaint();
    }

    private void refreshWeaponStats() {
        if (model.document().mode != VoxDocument.Mode.WEAPON) {
            statsArea.setText("");
            return;
        }
        WeaponParts.Stats s = model.weaponStats();
        StringBuilder sb = new StringBuilder();
        sb.append(String.format("damage   %7.2f%n", s.damage));
        sb.append(String.format("impact   %7.2f%n", s.impact));
        sb.append(String.format("recoil   %7.2f%n", s.recoil));
        sb.append(String.format("handling %7.2f%n", s.handling));
        sb.append(String.format("weight   %7.2f%n", s.weight));
        sb.append(String.format("optic    %7.2f%n", s.optic));
        sb.append("fire     ").append(s.fireMode).append('\n');
        sb.append("hitscan  ").append(s.hitscan ? 1 : 0).append('\n');
        sb.append("parts    ").append(s.partCounts);
        statsArea.setText(sb.toString());
    }

    private void syncWeaponCombos() {
        syncing = true;
        try {
            partBox.setSelectedItem(WeaponParts.name(model.activePart()));
            if (model.document().caliber != null) calBox.setSelectedItem(model.document().caliber);
            if (model.document().ammoId != null) ammoBox.setSelectedItem(model.document().ammoId);
            if (model.document().fireMode != null) fireBox.setSelectedItem(model.document().fireMode);
        } finally {
            syncing = false;
        }
    }

    @Override
    public void documentChanged() {
        refreshWeaponVisibility();
        syncWeaponCombos();
        refreshWeaponStats();
    }

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
            partBox.setSelectedItem(WeaponParts.name(model.activePart()));
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
            refreshWeaponStats();
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
        public int getIconWidth() { return size; }

        @Override
        public int getIconHeight() { return size; }
    }
}
