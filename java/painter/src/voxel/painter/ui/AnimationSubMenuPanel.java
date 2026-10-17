package voxel.painter.ui;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.Dimension;
import java.awt.FlowLayout;
import java.awt.GridBagConstraints;
import java.awt.GridBagLayout;
import java.awt.Insets;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Locale;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JButton;
import javax.swing.JCheckBox;
import javax.swing.JComboBox;
import javax.swing.JDialog;
import javax.swing.JFileChooser;
import javax.swing.JLabel;
import javax.swing.JOptionPane;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JSlider;
import javax.swing.JSpinner;
import javax.swing.JTextArea;
import javax.swing.JTextField;
import javax.swing.SpinnerNumberModel;
import javax.swing.border.EmptyBorder;
import javax.swing.filechooser.FileNameExtensionFilter;
import voxel.painter.anim.AnimationClip;
import voxel.painter.grid.BlenderBridge;

/**
 * Dedicated Animation Sub-Menu and Modifier Panel for Voxel Painter.
 * Provides controls for Blender project action fetching, skeletal animation variables,
 * playback speed, motion scaling, cross-fading, additive weighting, and .anim.json export.
 */
public class AnimationSubMenuPanel extends JPanel {

    private final Path repoRoot;
    private AnimationClip baseClip;
    private AnimationClip activeClip;

    private final JComboBox<String> actionSelectBox;
    private final JTextField txtId;
    private final JTextField txtName;
    private final JTextField txtDuration;
    private final JSpinner spinFps;
    private final JCheckBox chkLoop;
    private final JComboBox<String> cmbRigFilter;

    private final JSlider sliderSpeed;
    private final JLabel lblSpeedVal;
    private final JSlider sliderCrossFade;
    private final JLabel lblCrossFadeVal;
    private final JSlider sliderWeight;
    private final JLabel lblWeightVal;
    private final JSlider sliderMotionScale;
    private final JLabel lblMotionScaleVal;

    private final JTextArea txtStats;
    private final JLabel lblStatus;

    public AnimationSubMenuPanel(Path repoRoot) {
        this.repoRoot = repoRoot;
        setLayout(new BorderLayout(8, 8));
        setBackground(PainterTheme.BG);
        setBorder(new EmptyBorder(12, 14, 12, 14));

        // ---------------------------------------------------------------------
        // 1. Header & Blender Fetch Bar
        // ---------------------------------------------------------------------
        JPanel topPanel = new JPanel();
        topPanel.setLayout(new BoxLayout(topPanel, BoxLayout.Y_AXIS));
        topPanel.setOpaque(false);

        JLabel title = new JLabel("Animation Modifiers & Blender Project Fetch");
        title.setFont(PainterTheme.titleFont());
        title.setForeground(PainterTheme.ACCENT);
        topPanel.add(title);
        topPanel.add(Box.createVerticalStrut(4));

        JLabel blurb = new JLabel("Query Blender .blend projects, tune skeletal variables & motion curves, and export .anim.json");
        blurb.setFont(PainterTheme.uiFont());
        blurb.setForeground(PainterTheme.TEXT_DIM);
        topPanel.add(blurb);
        topPanel.add(Box.createVerticalStrut(8));

        JPanel fetchRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 8, 4));
        fetchRow.setOpaque(false);
        fetchRow.setBorder(PainterTheme.cardBorder("Blender Project Source"));

        fetchRow.add(new JLabel("Blend Action:"));
        actionSelectBox = new JComboBox<>();
        actionSelectBox.setPreferredSize(new Dimension(200, 26));
        fetchRow.add(actionSelectBox);

        JButton btnFetch = new JButton("Fetch Action from Blender");
        btnFetch.setBackground(PainterTheme.ACCENT_WARM);
        btnFetch.setForeground(PainterTheme.HEADER);
        btnFetch.addActionListener(e -> onFetchAction());
        fetchRow.add(btnFetch);

        JButton btnRefresh = new JButton("Refresh Actions");
        btnRefresh.addActionListener(e -> populateActions());
        fetchRow.add(btnRefresh);

        topPanel.add(fetchRow);
        add(topPanel, BorderLayout.NORTH);

        // ---------------------------------------------------------------------
        // 2. Middle Configuration: Variables & Modifiers
        // ---------------------------------------------------------------------
        JPanel centerPanel = new JPanel(new GridBagLayout());
        centerPanel.setOpaque(false);
        GridBagConstraints gbc = new GridBagConstraints();
        gbc.insets = new Insets(4, 4, 4, 4);
        gbc.fill = GridBagConstraints.BOTH;

        // --- Card A: Animation Variables ---
        JPanel varsCard = new JPanel(new GridBagLayout());
        varsCard.setOpaque(false);
        varsCard.setBorder(PainterTheme.cardBorder("Variables & Metadata"));

        GridBagConstraints vc = new GridBagConstraints();
        vc.insets = new Insets(4, 6, 4, 6);
        vc.fill = GridBagConstraints.HORIZONTAL;
        vc.anchor = GridBagConstraints.WEST;

        // Row 0: ID
        vc.gridx = 0; vc.gridy = 0; vc.weightx = 0.0;
        varsCard.add(new JLabel("Clip ID:"), vc);
        vc.gridx = 1; vc.gridy = 0; vc.weightx = 1.0;
        txtId = new JTextField(16);
        txtId.setBackground(PainterTheme.BG_INPUT);
        txtId.setForeground(PainterTheme.TEXT);
        txtId.setCaretColor(PainterTheme.TEXT);
        varsCard.add(txtId, vc);

        // Row 1: Name
        vc.gridx = 0; vc.gridy = 1; vc.weightx = 0.0;
        varsCard.add(new JLabel("Clip Name:"), vc);
        vc.gridx = 1; vc.gridy = 1; vc.weightx = 1.0;
        txtName = new JTextField(20);
        txtName.setBackground(PainterTheme.BG_INPUT);
        txtName.setForeground(PainterTheme.TEXT);
        txtName.setCaretColor(PainterTheme.TEXT);
        varsCard.add(txtName, vc);

        // Row 2: Duration
        vc.gridx = 0; vc.gridy = 2; vc.weightx = 0.0;
        varsCard.add(new JLabel("Duration (sec):"), vc);
        vc.gridx = 1; vc.gridy = 2; vc.weightx = 1.0;
        txtDuration = new JTextField(8);
        txtDuration.setBackground(PainterTheme.BG_INPUT);
        txtDuration.setForeground(PainterTheme.TEXT);
        txtDuration.setCaretColor(PainterTheme.TEXT);
        varsCard.add(txtDuration, vc);

        // Row 3: Frame Rate
        vc.gridx = 0; vc.gridy = 3; vc.weightx = 0.0;
        varsCard.add(new JLabel("Frame Rate (FPS):"), vc);
        vc.gridx = 1; vc.gridy = 3; vc.weightx = 1.0;
        spinFps = new JSpinner(new SpinnerNumberModel(30.0, 10.0, 120.0, 1.0));
        varsCard.add(spinFps, vc);

        // Row 4: Loop & Target Rig
        vc.gridx = 0; vc.gridy = 4; vc.weightx = 0.0;
        varsCard.add(new JLabel("Target Rig:"), vc);
        vc.gridx = 1; vc.gridy = 4; vc.weightx = 1.0;
        JPanel rigLoopRow = new JPanel(new FlowLayout(FlowLayout.LEFT, 6, 0));
        rigLoopRow.setOpaque(false);
        cmbRigFilter = new JComboBox<>(new String[]{"All Bones", "Humanoid Only", "Weapon Only"});
        rigLoopRow.add(cmbRigFilter);
        chkLoop = new JCheckBox("Loop Cycle", true);
        chkLoop.setOpaque(false);
        chkLoop.setForeground(PainterTheme.TEXT);
        rigLoopRow.add(chkLoop);
        varsCard.add(rigLoopRow, vc);

        gbc.gridx = 0; gbc.gridy = 0; gbc.weightx = 0.5; gbc.weighty = 0.6;
        centerPanel.add(varsCard, gbc);

        // --- Card B: Modifiers (Sliders) ---
        JPanel modsCard = new JPanel(new GridBagLayout());
        modsCard.setOpaque(false);
        modsCard.setBorder(PainterTheme.cardBorder("Modifiers & Curves"));

        GridBagConstraints mc = new GridBagConstraints();
        mc.insets = new Insets(4, 6, 4, 6);
        mc.fill = GridBagConstraints.HORIZONTAL;

        // Modifier 1: Playback Speed
        mc.gridx = 0; mc.gridy = 0; mc.weightx = 0.3;
        modsCard.add(new JLabel("Playback Speed:"), mc);
        mc.gridx = 1; mc.gridy = 0; mc.weightx = 0.5;
        sliderSpeed = new JSlider(10, 300, 100);
        sliderSpeed.setOpaque(false);
        modsCard.add(sliderSpeed, mc);
        mc.gridx = 2; mc.gridy = 0; mc.weightx = 0.2;
        lblSpeedVal = new JLabel("1.00x");
        lblSpeedVal.setFont(PainterTheme.monoFont());
        modsCard.add(lblSpeedVal, mc);
        sliderSpeed.addChangeListener(e -> lblSpeedVal.setText(String.format(Locale.ROOT, "%.2fx", sliderSpeed.getValue() / 100.0)));

        // Modifier 2: Motion Scale / Amplitude
        mc.gridx = 0; mc.gridy = 1; mc.weightx = 0.3;
        modsCard.add(new JLabel("Motion Amplitude:"), mc);
        mc.gridx = 1; mc.gridy = 1; mc.weightx = 0.5;
        sliderMotionScale = new JSlider(10, 200, 100);
        sliderMotionScale.setOpaque(false);
        modsCard.add(sliderMotionScale, mc);
        mc.gridx = 2; mc.gridy = 1; mc.weightx = 0.2;
        lblMotionScaleVal = new JLabel("1.00x");
        lblMotionScaleVal.setFont(PainterTheme.monoFont());
        modsCard.add(lblMotionScaleVal, mc);
        sliderMotionScale.addChangeListener(e -> lblMotionScaleVal.setText(String.format(Locale.ROOT, "%.2fx", sliderMotionScale.getValue() / 100.0)));

        // Modifier 3: Additive Weight
        mc.gridx = 0; mc.gridy = 2; mc.weightx = 0.3;
        modsCard.add(new JLabel("Additive Weight:"), mc);
        mc.gridx = 1; mc.gridy = 2; mc.weightx = 0.5;
        sliderWeight = new JSlider(0, 100, 100);
        sliderWeight.setOpaque(false);
        modsCard.add(sliderWeight, mc);
        mc.gridx = 2; mc.gridy = 2; mc.weightx = 0.2;
        lblWeightVal = new JLabel("1.00");
        lblWeightVal.setFont(PainterTheme.monoFont());
        modsCard.add(lblWeightVal, mc);
        sliderWeight.addChangeListener(e -> lblWeightVal.setText(String.format(Locale.ROOT, "%.2f", sliderWeight.getValue() / 100.0)));

        // Modifier 4: Cross-Fade Time
        mc.gridx = 0; mc.gridy = 3; mc.weightx = 0.3;
        modsCard.add(new JLabel("Cross-Fade Window:"), mc);
        mc.gridx = 1; mc.gridy = 3; mc.weightx = 0.5;
        sliderCrossFade = new JSlider(0, 100, 20);
        sliderCrossFade.setOpaque(false);
        modsCard.add(sliderCrossFade, mc);
        mc.gridx = 2; mc.gridy = 3; mc.weightx = 0.2;
        lblCrossFadeVal = new JLabel("0.20s");
        lblCrossFadeVal.setFont(PainterTheme.monoFont());
        modsCard.add(lblCrossFadeVal, mc);
        sliderCrossFade.addChangeListener(e -> lblCrossFadeVal.setText(String.format(Locale.ROOT, "%.2fs", sliderCrossFade.getValue() / 100.0)));

        gbc.gridx = 1; gbc.gridy = 0; gbc.weightx = 0.5; gbc.weighty = 0.6;
        centerPanel.add(modsCard, gbc);

        // --- Card C: Track Inspection & Telemetry ---
        JPanel statsCard = new JPanel(new BorderLayout(4, 4));
        statsCard.setOpaque(false);
        statsCard.setBorder(PainterTheme.cardBorder("Clip Inspection"));

        txtStats = new JTextArea(4, 40);
        txtStats.setFont(PainterTheme.monoFont());
        txtStats.setBackground(PainterTheme.BG_INPUT);
        txtStats.setForeground(PainterTheme.TEXT);
        txtStats.setEditable(false);
        txtStats.setText("No animation clip loaded.\nFetch an action from Blender above or select a preset.");
        statsCard.add(new JScrollPane(txtStats), BorderLayout.CENTER);

        gbc.gridx = 0; gbc.gridy = 1; gbc.gridwidth = 2; gbc.weightx = 1.0; gbc.weighty = 0.4;
        centerPanel.add(statsCard, gbc);

        add(centerPanel, BorderLayout.CENTER);

        // ---------------------------------------------------------------------
        // 3. Bottom Button & Status Bar
        // ---------------------------------------------------------------------
        JPanel bottomPanel = new JPanel(new BorderLayout(8, 4));
        bottomPanel.setOpaque(false);

        lblStatus = new JLabel("Ready.");
        lblStatus.setFont(PainterTheme.uiFont());
        lblStatus.setForeground(PainterTheme.TEXT_DIM);
        bottomPanel.add(lblStatus, BorderLayout.WEST);

        JPanel btnBar = new JPanel(new FlowLayout(FlowLayout.RIGHT, 8, 0));
        btnBar.setOpaque(false);

        JButton btnApply = new JButton("Apply Modifiers");
        btnApply.setBackground(PainterTheme.ACCENT);
        btnApply.setForeground(PainterTheme.HEADER);
        btnApply.addActionListener(e -> onApplyModifiers());
        btnBar.add(btnApply);

        JButton btnReset = new JButton("Reset Modifiers");
        btnReset.addActionListener(e -> onResetModifiers());
        btnBar.add(btnReset);

        JButton btnExport = new JButton("Export .anim.json");
        btnExport.setBackground(PainterTheme.OK);
        btnExport.setForeground(PainterTheme.HEADER);
        btnExport.addActionListener(e -> onExportClip());
        btnBar.add(btnExport);

        bottomPanel.add(btnBar, BorderLayout.EAST);
        add(bottomPanel, BorderLayout.SOUTH);

        // Initial actions query
        populateActions();
    }

    private void populateActions() {
        actionSelectBox.removeAllItems();
        List<String> actions = BlenderBridge.listActions(repoRoot, null);
        for (String a : actions) {
            actionSelectBox.addItem(a);
        }
        if (actionSelectBox.getItemCount() > 0) {
            actionSelectBox.setSelectedIndex(0);
        }
        lblStatus.setText("Discovered " + actions.size() + " actions in Blender project.");
    }

    private void onFetchAction() {
        String actionName = (String) actionSelectBox.getSelectedItem();
        if (actionName == null || actionName.isBlank()) {
            JOptionPane.showMessageDialog(this, "Please select an action from the dropdown.", "No action selected", JOptionPane.WARNING_MESSAGE);
            return;
        }

        try {
            lblStatus.setText("Fetching '" + actionName + "' from Blender...");
            AnimationClip clip = BlenderBridge.fetchAnimation(repoRoot, null, actionName);
            if (clip != null) {
                setClip(clip);
                lblStatus.setText("Successfully fetched '" + actionName + "' (" + clip.tracks.size() + " tracks).");
            }
        } catch (Exception ex) {
            lblStatus.setText("Failed to fetch action: " + ex.getMessage());
            JOptionPane.showMessageDialog(this, "Could not fetch action: " + ex.getMessage(), "Fetch Error", JOptionPane.ERROR_MESSAGE);
        }
    }

    public void setClip(AnimationClip clip) {
        this.baseClip = clip.copy();
        this.activeClip = clip;

        txtId.setText(clip.id);
        txtName.setText(clip.name);
        txtDuration.setText(String.format(Locale.ROOT, "%.4f", clip.durationSec));
        spinFps.setValue(clip.frameRate);
        chkLoop.setSelected(clip.loop);

        onResetModifiers();
        updateStats();
    }

    public AnimationClip getActiveClip() {
        return activeClip;
    }

    private void onApplyModifiers() {
        if (baseClip == null) {
            JOptionPane.showMessageDialog(this, "No clip currently loaded. Fetch an action from Blender first.", "No Clip", JOptionPane.WARNING_MESSAGE);
            return;
        }

        double speed = sliderSpeed.getValue() / 100.0;
        double motion = sliderMotionScale.getValue() / 100.0;
        double weight = sliderWeight.getValue() / 100.0;
        double crossFade = sliderCrossFade.getValue() / 100.0;
        double fps = ((Number) spinFps.getValue()).doubleValue();
        boolean loop = chkLoop.isSelected();
        String id = txtId.getText();
        String name = txtName.getText();
        String rig = (String) cmbRigFilter.getSelectedItem();

        activeClip = baseClip.applyModifiers(speed, motion, weight, crossFade, fps, loop, id, name, rig);
        txtDuration.setText(String.format(Locale.ROOT, "%.4f", activeClip.durationSec));
        lblStatus.setText("Applied modifiers: speed=" + String.format(Locale.ROOT, "%.2fx", speed)
            + " motion=" + String.format(Locale.ROOT, "%.2fx", motion)
            + " weight=" + String.format(Locale.ROOT, "%.2f", weight)
            + " (" + activeClip.tracks.size() + " active tracks).");
        updateStats();
    }

    private void onResetModifiers() {
        sliderSpeed.setValue(100);
        sliderMotionScale.setValue(100);
        sliderWeight.setValue(100);
        sliderCrossFade.setValue(20);
        lblSpeedVal.setText("1.00x");
        lblMotionScaleVal.setText("1.00x");
        lblWeightVal.setText("1.00");
        lblCrossFadeVal.setText("0.20s");

        if (baseClip != null) {
            activeClip = baseClip.copy();
            txtDuration.setText(String.format(Locale.ROOT, "%.4f", activeClip.durationSec));
            updateStats();
        }
    }

    private void updateStats() {
        if (activeClip == null) {
            txtStats.setText("No animation clip loaded.");
            return;
        }

        int totalKeyframes = 0;
        StringBuilder sb = new StringBuilder();
        sb.append("ID: ").append(activeClip.id).append(" | Name: \"").append(activeClip.name).append("\"\n");
        sb.append(String.format(Locale.ROOT, "Duration: %.4fs | FPS: %.1f | Loop: %s | Tracks: %d\n",
                activeClip.durationSec, activeClip.frameRate, activeClip.loop, activeClip.tracks.size()));

        sb.append("Tracks: ");
        for (int i = 0; i < activeClip.tracks.size(); i++) {
            AnimationClip.Track t = activeClip.tracks.get(i);
            totalKeyframes += t.frames.size();
            sb.append(t.bone).append("(").append(t.frames.size()).append("k)");
            if (i + 1 < activeClip.tracks.size()) sb.append(", ");
        }
        sb.append("\nTotal keyframe nodes: ").append(totalKeyframes);
        txtStats.setText(sb.toString());
    }

    private void onExportClip() {
        if (activeClip == null) {
            JOptionPane.showMessageDialog(this, "No clip to export. Please fetch or load an animation first.", "Export Error", JOptionPane.WARNING_MESSAGE);
            return;
        }

        // Make sure latest inputs are applied
        onApplyModifiers();

        JFileChooser fc = new JFileChooser();
        Path animsDir = repoRoot != null ? repoRoot.resolve("data").resolve("anims") : null;
        if (animsDir != null && Files.exists(animsDir)) {
            fc.setCurrentDirectory(animsDir.toFile());
        }
        String defaultName = (activeClip.id != null && !activeClip.id.isBlank()) ? activeClip.id : "clip";
        if (!defaultName.endsWith(".anim.json")) {
            defaultName += ".anim.json";
        }
        fc.setSelectedFile(new File(defaultName));
        fc.setFileFilter(new FileNameExtensionFilter("Animation Clip (.anim.json)", "json"));

        if (fc.showSaveDialog(this) == JFileChooser.APPROVE_OPTION) {
            File sel = fc.getSelectedFile();
            Path outPath = sel.toPath();
            if (!outPath.getFileName().toString().endsWith(".anim.json") && !outPath.getFileName().toString().endsWith(".json")) {
                outPath = outPath.resolveSibling(outPath.getFileName().toString() + ".anim.json");
            }
            try {
                activeClip.save(outPath);
                lblStatus.setText("Exported animation clip to: " + outPath.getFileName());
                JOptionPane.showMessageDialog(this, "Saved animation clip to:\n" + outPath, "Export Complete", JOptionPane.INFORMATION_MESSAGE);
            } catch (Exception ex) {
                lblStatus.setText("Export failed: " + ex.getMessage());
                JOptionPane.showMessageDialog(this, "Export failed: " + ex.getMessage(), "Error", JOptionPane.ERROR_MESSAGE);
            }
        }
    }

    /**
     * Helper to open the Animation Sub-Menu modal dialog.
     */
    public static JDialog showDialog(Component parent, Path repoRoot, AnimationClip initialClip) {
        Component root = javax.swing.SwingUtilities.getRoot(parent);
        java.awt.Frame owner = root instanceof java.awt.Frame ? (java.awt.Frame) root : null;
        JDialog dialog = new JDialog(owner, "Animation Sub-Menu & Modifiers", true);
        dialog.setDefaultCloseOperation(JDialog.DISPOSE_ON_CLOSE);

        AnimationSubMenuPanel panel = new AnimationSubMenuPanel(repoRoot);
        if (initialClip != null) {
            panel.setClip(initialClip);
        }

        dialog.getContentPane().setLayout(new BorderLayout());
        dialog.getContentPane().add(panel, BorderLayout.CENTER);
        dialog.pack();
        dialog.setMinimumSize(new Dimension(680, 520));
        dialog.setLocationRelativeTo(parent);
        dialog.setVisible(true);
        return dialog;
    }
}
