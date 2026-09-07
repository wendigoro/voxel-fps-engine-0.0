package voxel.painter.ui;

import java.awt.FlowLayout;
import java.awt.GridBagConstraints;
import java.awt.GridBagLayout;
import java.awt.Insets;
import javax.swing.BorderFactory;
import javax.swing.JButton;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JSpinner;
import javax.swing.SpinnerNumberModel;
import voxel.painter.grid.SkyAndCharacter;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxelGrid;

public final class ModeExtrasPanel extends JPanel implements PainterModel.Listener {
    private final PainterModel model;
    private final JSpinner segU;
    private final JSpinner segV;
    private final JSpinner moonX, moonY, moonZ, moonI;
    private final JSpinner feetX, feetY, feetZ;
    private final JLabel modeLabel;
    private boolean syncing;

    public ModeExtrasPanel(PainterModel model) {
        this.model = model;
        setLayout(new GridBagLayout());
        setBorder(BorderFactory.createTitledBorder("Mode extras"));
        GridBagConstraints c = new GridBagConstraints();
        c.insets = new Insets(2, 4, 2, 4);
        c.anchor = GridBagConstraints.WEST;

        modeLabel = new JLabel();
        c.gridx = 0;
        c.gridy = 0;
        c.gridwidth = 6;
        add(modeLabel, c);
        c.gridwidth = 1;

        c.gridy = 1;
        c.gridx = 0;
        add(new JLabel("seg_u"), c);
        segU = new JSpinner(new SpinnerNumberModel(28, 1, 128, 1));
        c.gridx = 1;
        add(segU, c);
        c.gridx = 2;
        add(new JLabel("seg_v"), c);
        segV = new JSpinner(new SpinnerNumberModel(14, 1, 128, 1));
        c.gridx = 3;
        add(segV, c);

        c.gridy = 2;
        c.gridx = 0;
        add(new JLabel("moon dir"), c);
        moonX = new JSpinner(new SpinnerNumberModel(0.32, -1.0, 1.0, 0.01));
        moonY = new JSpinner(new SpinnerNumberModel(0.82, -1.0, 1.0, 0.01));
        moonZ = new JSpinner(new SpinnerNumberModel(-0.48, -1.0, 1.0, 0.01));
        moonI = new JSpinner(new SpinnerNumberModel(0.95, 0.0, 2.0, 0.05));
        JPanel moon = new JPanel(new FlowLayout(FlowLayout.LEFT, 2, 0));
        moon.add(moonX);
        moon.add(moonY);
        moon.add(moonZ);
        moon.add(new JLabel("I"));
        moon.add(moonI);
        c.gridx = 1;
        c.gridwidth = 5;
        add(moon, c);
        c.gridwidth = 1;

        c.gridy = 3;
        c.gridx = 0;
        add(new JLabel("feet"), c);
        feetX = new JSpinner(new SpinnerNumberModel(8, -128, 128, 1));
        feetY = new JSpinner(new SpinnerNumberModel(0, -128, 128, 1));
        feetZ = new JSpinner(new SpinnerNumberModel(4, -128, 128, 1));
        JPanel feet = new JPanel(new FlowLayout(FlowLayout.LEFT, 2, 0));
        feet.add(feetX);
        feet.add(feetY);
        feet.add(feetZ);
        c.gridx = 1;
        c.gridwidth = 3;
        add(feet, c);
        c.gridwidth = 1;

        JButton bakeSky = new JButton("Bake sky tiles");
        bakeSky.addActionListener(e -> {
            push();
            VoxDocument doc = model.document();
            SkyAndCharacter.paintSkyTiles(
                    doc.grid, doc.segU, doc.segV,
                    doc.moonDirX, doc.moonDirY, doc.moonDirZ, doc.moonIntensity);
            model.markDirty();
        });
        JButton bakeChar = new JButton("Place character");
        bakeChar.addActionListener(e -> {
            push();
            VoxDocument doc = model.document();
            doc.grid.clear();
            SkyAndCharacter.paintCharacter(doc.grid, doc.feetX, doc.feetY, doc.feetZ);
            model.markDirty();
        });
        c.gridy = 4;
        c.gridx = 0;
        c.gridwidth = 2;
        add(bakeSky, c);
        c.gridx = 2;
        add(bakeChar, c);

        for (JSpinner s : new JSpinner[] {segU, segV, moonX, moonY, moonZ, moonI, feetX, feetY, feetZ}) {
            s.addChangeListener(ev -> push());
        }
        model.addListener(this);
        pull();
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
            modeLabel.setText(
                    "Active: " + doc.modeName()
                            + "  unit=" + doc.unit
                            + " voxel_size=" + VoxelGrid.VOXEL_SIZE
                            + "  solids=" + doc.grid.solidCount());
            segU.setValue(doc.segU);
            segV.setValue(doc.segV);
            moonX.setValue((double) doc.moonDirX);
            moonY.setValue((double) doc.moonDirY);
            moonZ.setValue((double) doc.moonDirZ);
            moonI.setValue((double) doc.moonIntensity);
            feetX.setValue(doc.feetX);
            feetY.setValue(doc.feetY);
            feetZ.setValue(doc.feetZ);
            boolean sky = doc.mode == VoxDocument.Mode.SKY;
            boolean character = doc.mode == VoxDocument.Mode.CHARACTER;
            segU.setEnabled(sky);
            segV.setEnabled(sky);
            moonX.setEnabled(sky);
            moonY.setEnabled(sky);
            moonZ.setEnabled(sky);
            moonI.setEnabled(sky);
            feetX.setEnabled(character);
            feetY.setEnabled(character);
            feetZ.setEnabled(character);
        } finally {
            syncing = false;
        }
    }

    @Override
    public void documentChanged() {
        pull();
    }

    @Override
    public void toolsChanged() {}

    @Override
    public void sliceChanged() {}
}
