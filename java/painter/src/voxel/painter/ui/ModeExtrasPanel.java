package voxel.painter.ui;

import java.awt.FlowLayout;
import java.awt.Font;
import java.awt.GridBagConstraints;
import java.awt.GridBagLayout;
import java.awt.Insets;
import javax.swing.JButton;
import javax.swing.JComboBox;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JSpinner;
import javax.swing.SpinnerNumberModel;
import voxel.painter.grid.SkyAndCharacter;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxelGrid;
import voxel.painter.grid.WeaponParts;

/** Bottom strip: sky/character extras + weapon caliber/ammo/fire when in weapon mode. */
public final class ModeExtrasPanel extends JPanel implements PainterModel.Listener {
    private final PainterModel model;
    private final JSpinner segU;
    private final JSpinner segV;
    private final JSpinner moonX, moonY, moonZ, moonI;
    private final JSpinner feetX, feetY, feetZ;
    private final JLabel modeLabel;
    private final JComboBox<String> calBox;
    private final JComboBox<String> ammoBox;
    private final JComboBox<String> fireBox;
    private final JLabel weaponStats;
    private final JPanel skyRow;
    private final JPanel charRow;
    private final JPanel weaponRow;
    private final JButton bakeSky;
    private final JButton bakeChar;
    private final JButton bakeWeapon;
    private boolean syncing;

    public ModeExtrasPanel(PainterModel model) {
        this.model = model;
        setLayout(new GridBagLayout());
        setBackground(PainterTheme.BG_PANEL);
        setBorder(PainterTheme.cardBorder("Mode extras"));
        GridBagConstraints c = new GridBagConstraints();
        c.insets = new Insets(2, 4, 2, 4);
        c.anchor = GridBagConstraints.WEST;

        modeLabel = new JLabel();
        modeLabel.setForeground(PainterTheme.TEXT);
        modeLabel.setFont(PainterTheme.uiFont().deriveFont(Font.BOLD));
        c.gridx = 0;
        c.gridy = 0;
        c.gridwidth = 8;
        add(modeLabel, c);
        c.gridwidth = 1;

        skyRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 4, 0));
        skyRow.setOpaque(false);
        skyRow.add(dim("seg_u"));
        segU = new JSpinner(new SpinnerNumberModel(28, 1, 128, 1));
        skyRow.add(segU);
        skyRow.add(dim("seg_v"));
        segV = new JSpinner(new SpinnerNumberModel(14, 1, 128, 1));
        skyRow.add(segV);
        skyRow.add(dim("moon"));
        moonX = new JSpinner(new SpinnerNumberModel(0.32, -1.0, 1.0, 0.01));
        moonY = new JSpinner(new SpinnerNumberModel(0.82, -1.0, 1.0, 0.01));
        moonZ = new JSpinner(new SpinnerNumberModel(-0.48, -1.0, 1.0, 0.01));
        moonI = new JSpinner(new SpinnerNumberModel(0.95, 0.0, 2.0, 0.05));
        skyRow.add(moonX);
        skyRow.add(moonY);
        skyRow.add(moonZ);
        skyRow.add(dim("I"));
        skyRow.add(moonI);
        c.gridy = 1;
        c.gridx = 0;
        c.gridwidth = 8;
        add(skyRow, c);

        charRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 4, 0));
        charRow.setOpaque(false);
        charRow.add(dim("feet"));
        feetX = new JSpinner(new SpinnerNumberModel(8, -128, 128, 1));
        feetY = new JSpinner(new SpinnerNumberModel(0, -128, 128, 1));
        feetZ = new JSpinner(new SpinnerNumberModel(4, -128, 128, 1));
        charRow.add(feetX);
        charRow.add(feetY);
        charRow.add(feetZ);
        c.gridy = 2;
        add(charRow, c);

        weaponRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 4, 0));
        weaponRow.setOpaque(false);
        weaponRow.add(dim("caliber"));
        calBox = new JComboBox<>(new String[]{"light", "medium", "heavy", "energy"});
        weaponRow.add(calBox);
        weaponRow.add(dim("ammo"));
        ammoBox = new JComboBox<>(new String[]{
                "light_fmj", "light_hp", "light_ap",
                "medium_fmj", "medium_tracer", "medium_shred",
                "heavy_fmj", "heavy_he",
                "energy_bolt", "energy_pierce"
        });
        weaponRow.add(ammoBox);
        weaponRow.add(dim("fire"));
        fireBox = new JComboBox<>(new String[]{"semi", "auto", "bolt"});
        weaponRow.add(fireBox);
        weaponStats = new JLabel(" ");
        weaponStats.setForeground(PainterTheme.MODE_WEAPON);
        weaponStats.setFont(PainterTheme.monoFont());
        weaponRow.add(weaponStats);
        c.gridy = 3;
        add(weaponRow, c);
        c.gridwidth = 1;

        bakeSky = new JButton("Bake sky tiles");
        bakeSky.addActionListener(e -> {
            push();
            VoxDocument doc = model.document();
            SkyAndCharacter.paintSkyTiles(
                    doc.grid, doc.segU, doc.segV,
                    doc.moonDirX, doc.moonDirY, doc.moonDirZ, doc.moonIntensity);
            model.markDirty();
        });
        bakeChar = new JButton("Place character");
        bakeChar.addActionListener(e -> {
            push();
            VoxDocument doc = model.document();
            doc.grid.clear();
            SkyAndCharacter.paintCharacter(doc.grid, doc.feetX, doc.feetY, doc.feetZ);
            model.markDirty();
        });
        bakeWeapon = new JButton("Bake starter rifle");
        bakeWeapon.addActionListener(e -> model.bakeStarterWeapon());

        JPanel bakes = new JPanel(new FlowLayout(FlowLayout.LEFT, 6, 0));
        bakes.setOpaque(false);
        bakes.add(bakeSky);
        bakes.add(bakeChar);
        bakes.add(bakeWeapon);
        c.gridy = 4;
        c.gridx = 0;
        c.gridwidth = 8;
        add(bakes, c);

        calBox.addActionListener(e -> {
            if (!syncing) model.setCaliber((String) calBox.getSelectedItem());
        });
        ammoBox.addActionListener(e -> {
            if (!syncing) model.setAmmoId((String) ammoBox.getSelectedItem());
        });
        fireBox.addActionListener(e -> {
            if (!syncing) model.setFireMode((String) fireBox.getSelectedItem());
        });

        for (JSpinner s : new JSpinner[] {segU, segV, moonX, moonY, moonZ, moonI, feetX, feetY, feetZ}) {
            s.addChangeListener(ev -> push());
        }
        model.addListener(this);
        pull();
    }

    private static JLabel dim(String t) {
        JLabel l = new JLabel(t);
        l.setForeground(PainterTheme.TEXT_DIM);
        return l;
    }

    private void push() {
        if (syncing) return;
        VoxDocument doc = model.document();
        doc.segU = (Integer) segU.getValue();
        doc.segV = (Integer) segV.getValue();
        doc.moonDirX = ((Number) moonX.getValue()).floatValue();
        doc.moonDirY = ((Number) moonY.getValue()).floatValue();
        doc.moonDirZ = ((Number) moonZ.getValue()).floatValue();
        doc.moonIntensity = ((Number) moonI.getValue()).floatValue();
        doc.feetX = (Integer) feetX.getValue();
        doc.feetY = (Integer) feetY.getValue();
        doc.feetZ = (Integer) feetZ.getValue();
        model.markDirty();
    }

    private void pull() {
        syncing = true;
        try {
            VoxDocument doc = model.document();
            boolean sky = doc.mode == VoxDocument.Mode.SKY;
            boolean character = doc.mode == VoxDocument.Mode.CHARACTER;
            boolean weapon = doc.mode == VoxDocument.Mode.WEAPON;

            String extra = "";
            if (weapon) {
                WeaponParts.Stats st = model.weaponStats();
                extra = String.format(
                        "  cal=%s ammo=%s fire=%s  dmg=%.1f opt=%.1f",
                        doc.caliber, doc.ammoId, doc.fireMode, st.damage, st.optic);
                weaponStats.setText(String.format(
                        "stats dmg=%.1f imp=%.1f rec=%.1f hnd=%.1f w=%.1f opt=%.1f",
                        st.damage, st.impact, st.recoil, st.handling, st.weight, st.optic));
                if (doc.caliber != null) calBox.setSelectedItem(doc.caliber);
                if (doc.ammoId != null) ammoBox.setSelectedItem(doc.ammoId);
                if (doc.fireMode != null) fireBox.setSelectedItem(doc.fireMode);
            } else {
                weaponStats.setText(" ");
            }

            modeLabel.setText(
                    "Active: " + PainterTheme.modeTitle(doc.mode)
                            + "   unit=" + doc.unit
                            + "   voxel_size=" + VoxelGrid.VOXEL_SIZE
                            + "   solids=" + doc.grid.solidCount()
                            + extra);
            modeLabel.setForeground(PainterTheme.modeColor(doc.mode));

            segU.setValue(doc.segU);
            segV.setValue(doc.segV);
            moonX.setValue((double) doc.moonDirX);
            moonY.setValue((double) doc.moonDirY);
            moonZ.setValue((double) doc.moonDirZ);
            moonI.setValue((double) doc.moonIntensity);
            feetX.setValue(doc.feetX);
            feetY.setValue(doc.feetY);
            feetZ.setValue(doc.feetZ);

            skyRow.setVisible(sky);
            charRow.setVisible(character);
            weaponRow.setVisible(weapon);
            bakeSky.setEnabled(sky);
            bakeChar.setEnabled(character);
            bakeWeapon.setEnabled(weapon || true); // always allow jump-to-weapon bake
            bakeWeapon.setVisible(true);
        } finally {
            syncing = false;
        }
    }

    @Override
    public void documentChanged() {
        pull();
    }

    @Override
    public void toolsChanged() {
        if (model.document().mode == VoxDocument.Mode.WEAPON) pull();
    }

    @Override
    public void sliceChanged() {}
}
