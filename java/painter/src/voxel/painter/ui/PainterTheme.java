package voxel.painter.ui;

import java.awt.Color;
import java.awt.Font;
import javax.swing.BorderFactory;
import javax.swing.UIManager;
import javax.swing.border.Border;
import javax.swing.border.CompoundBorder;
import javax.swing.border.EmptyBorder;
import javax.swing.border.LineBorder;
import javax.swing.border.TitledBorder;
import voxel.painter.grid.VoxDocument;

/** Distinct dark chrome for the Voxel Painter Swing shell (separate from the Vulkan engine). */
public final class PainterTheme {
    private PainterTheme() {}

    public static final Color BG = new Color(0x12, 0x14, 0x18);
    public static final Color BG_RAISED = new Color(0x1A, 0x1E, 0x26);
    public static final Color BG_PANEL = new Color(0x16, 0x1A, 0x22);
    public static final Color BG_INPUT = new Color(0x0E, 0x10, 0x14);
    public static final Color BORDER = new Color(0x2E, 0x36, 0x44);
    public static final Color BORDER_FOCUS = new Color(0x4A, 0x7A, 0xD4);
    public static final Color TEXT = new Color(0xE6, 0xEC, 0xF4);
    public static final Color TEXT_DIM = new Color(0x9A, 0xA4, 0xB4);
    public static final Color ACCENT = new Color(0x4C, 0x9A, 0xFF);
    public static final Color ACCENT_WARM = new Color(0xE0, 0x9A, 0x3C);
    public static final Color DANGER = new Color(0xE0, 0x5A, 0x5A);
    public static final Color OK = new Color(0x4C, 0xC0, 0x7A);
    public static final Color HEADER = new Color(0x0C, 0x0E, 0x12);
    public static final Color STATUS_BG = new Color(0x0A, 0x0C, 0x10);
    public static final Color CANVAS = new Color(0x1E, 0x1E, 0x22);
    public static final Color GRID_A = new Color(0x2A, 0x2A, 0x30);
    public static final Color GRID_B = new Color(0x24, 0x24, 0x28);

    public static final Color MODE_MODEL = new Color(0x4C, 0x9A, 0xFF);
    public static final Color MODE_SKY = new Color(0x6A, 0x8C, 0xE8);
    public static final Color MODE_CHARACTER = new Color(0x5C, 0xC0, 0x9A);
    public static final Color MODE_WEAPON = new Color(0xE0, 0x8A, 0x3C);

    public static Font uiFont() {
        return new Font(Font.SANS_SERIF, Font.PLAIN, 12);
    }

    public static Font titleFont() {
        return new Font(Font.SANS_SERIF, Font.BOLD, 16);
    }

    public static Font monoFont() {
        return new Font(Font.MONOSPACED, Font.PLAIN, 11);
    }

    public static Color modeColor(VoxDocument.Mode mode) {
        return switch (mode) {
            case SKY -> MODE_SKY;
            case CHARACTER -> MODE_CHARACTER;
            case WEAPON -> MODE_WEAPON;
            default -> MODE_MODEL;
        };
    }

    public static String modeTitle(VoxDocument.Mode mode) {
        return switch (mode) {
            case SKY -> "Sky tiles";
            case CHARACTER -> "Character";
            case WEAPON -> "Weapon assembly";
            default -> "Model";
        };
    }

    public static String modeBlurb(VoxDocument.Mode mode) {
        return switch (mode) {
            case SKY -> "Hemisphere tile grid + moon bearing for engine sky";
            case CHARACTER -> "Unit-voxel body kit; feet anchor for submersion tests";
            case WEAPON -> "Part-tagged voxels → stats, caliber, ammo, fire mode export";
            default -> "General cubic unit occupancy editing";
        };
    }

    public static Border cardBorder(String title) {
        TitledBorder tb = BorderFactory.createTitledBorder(
                new LineBorder(BORDER, 1, true), title);
        tb.setTitleColor(TEXT_DIM);
        tb.setTitleFont(uiFont());
        return new CompoundBorder(tb, new EmptyBorder(6, 8, 8, 8));
    }

    public static Border cardBorder(String title, Color accent) {
        TitledBorder tb = BorderFactory.createTitledBorder(
                new LineBorder(accent.darker(), 1, true), title);
        tb.setTitleColor(accent);
        tb.setTitleFont(uiFont().deriveFont(Font.BOLD));
        return new CompoundBorder(tb, new EmptyBorder(6, 8, 8, 8));
    }

    public static void installLookAndFeel() {
        try {
            UIManager.setLookAndFeel(UIManager.getCrossPlatformLookAndFeelClassName());
        } catch (Exception ignored) {
        }
        // Flat dark defaults so nested Swing controls match the painter chrome.
        UIManager.put("control", BG_PANEL);
        UIManager.put("info", BG_RAISED);
        UIManager.put("nimbusBase", BG);
        UIManager.put("nimbusBlueGrey", BG_PANEL);
        UIManager.put("nimbusLightBackground", BG_INPUT);
        UIManager.put("text", TEXT);
        UIManager.put("Panel.background", BG);
        UIManager.put("Panel.foreground", TEXT);
        UIManager.put("Label.foreground", TEXT);
        UIManager.put("Label.background", BG);
        UIManager.put("Button.background", BG_RAISED);
        UIManager.put("Button.foreground", TEXT);
        UIManager.put("ToggleButton.background", BG_RAISED);
        UIManager.put("ToggleButton.foreground", TEXT);
        UIManager.put("RadioButton.background", BG_PANEL);
        UIManager.put("RadioButton.foreground", TEXT);
        UIManager.put("CheckBox.background", BG_PANEL);
        UIManager.put("CheckBox.foreground", TEXT);
        UIManager.put("ComboBox.background", BG_INPUT);
        UIManager.put("ComboBox.foreground", TEXT);
        UIManager.put("ComboBox.selectionBackground", ACCENT.darker());
        UIManager.put("ComboBox.selectionForeground", Color.WHITE);
        UIManager.put("List.background", BG_INPUT);
        UIManager.put("List.foreground", TEXT);
        UIManager.put("List.selectionBackground", ACCENT.darker());
        UIManager.put("List.selectionForeground", Color.WHITE);
        UIManager.put("TextField.background", BG_INPUT);
        UIManager.put("TextField.foreground", TEXT);
        UIManager.put("TextField.caretForeground", TEXT);
        UIManager.put("TextArea.background", BG_INPUT);
        UIManager.put("TextArea.foreground", TEXT);
        UIManager.put("TextArea.caretForeground", TEXT);
        UIManager.put("ScrollPane.background", BG);
        UIManager.put("Viewport.background", BG);
        UIManager.put("TabbedPane.background", BG);
        UIManager.put("TabbedPane.foreground", TEXT_DIM);
        UIManager.put("TabbedPane.selected", BG_RAISED);
        UIManager.put("TabbedPane.contentAreaColor", BG);
        UIManager.put("MenuBar.background", HEADER);
        UIManager.put("MenuBar.foreground", TEXT);
        UIManager.put("Menu.background", HEADER);
        UIManager.put("Menu.foreground", TEXT);
        UIManager.put("MenuItem.background", BG_RAISED);
        UIManager.put("MenuItem.foreground", TEXT);
        UIManager.put("PopupMenu.background", BG_RAISED);
        UIManager.put("PopupMenu.foreground", TEXT);
        UIManager.put("OptionPane.background", BG_PANEL);
        UIManager.put("OptionPane.messageForeground", TEXT);
        UIManager.put("ToolTip.background", BG_RAISED);
        UIManager.put("ToolTip.foreground", TEXT);
        UIManager.put("Slider.background", BG_PANEL);
        UIManager.put("Spinner.background", BG_INPUT);
        UIManager.put("Spinner.foreground", TEXT);
        UIManager.put("ScrollBar.thumb", BORDER);
        UIManager.put("ScrollBar.track", BG);
        UIManager.put("SplitPane.background", BG);
        UIManager.put("SplitPane.dividerSize", 8);
        UIManager.put("TitledBorder.titleColor", TEXT_DIM);
    }
}
