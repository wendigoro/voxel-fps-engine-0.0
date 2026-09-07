package voxel.painter;

import voxel.painter.filter.BitcrushUpscale;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.PaintTools;
import voxel.painter.grid.SkyAndCharacter;
import voxel.painter.grid.VoxelGrid;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxIO;

import java.nio.file.Files;
import java.nio.file.Path;

/** Headless smoke: tools, sky, character, IO, bitcrush 32x filter. */
public final class SmokeMain {
    public static void main(String[] args) throws Exception {
        Path root = Path.of(args.length > 0 ? args[0] : ".").toAbsolutePath().normalize();
        Path outDir = root.resolve("data").resolve("voxfmt");
        Files.createDirectories(outDir);

        // --- model tools ---
        VoxDocument model = new VoxDocument(VoxDocument.Mode.MODEL, 32, 24, 32);
        model.grid.assertCubicUnitInvariant();
        if (model.grid.unitSizeX() != model.grid.unitSizeZ()
                || model.grid.unitSizeY() != model.grid.unitSizeX()) {
            throw new IllegalStateException("unit size mismatch xyz");
        }

        PaintTools tools = new PaintTools();
        tools.shape = PaintTools.BrushShape.CUBE;
        tools.brushSize = 3;
        tools.matA = MaterialPalette.CONCRETE;
        tools.paintAt(model.grid, 8, 1, 8);
        tools.toggleAlternate();
        tools.matB = MaterialPalette.WOOD;
        tools.shape = PaintTools.BrushShape.SPHERE;
        tools.brushSize = 5;
        tools.paintAt(model.grid, 16, 4, 16);
        tools.useB = false;
        tools.matA = MaterialPalette.DIRT;
        tools.floodFill(model.grid, 0, 0, 0); // fill air floor region contact — may fill large air; limit by painting shell first
        // safer fill demo: paint a closed box then fill interior air pocket
        VoxDocument box = new VoxDocument(VoxDocument.Mode.MODEL, 16, 16, 16);
        tools.matA = MaterialPalette.SHEET_METAL;
        tools.shape = PaintTools.BrushShape.POINT;
        tools.brushSize = 1;
        for (int i = 2; i <= 6; i++) {
            for (int j = 2; j <= 6; j++) {
                box.grid.setMat(i, 2, j, MaterialPalette.SHEET_METAL);
                box.grid.setMat(i, 6, j, MaterialPalette.SHEET_METAL);
                box.grid.setMat(2, i, j, MaterialPalette.SHEET_METAL);
                box.grid.setMat(6, i, j, MaterialPalette.SHEET_METAL);
                box.grid.setMat(i, j, 2, MaterialPalette.SHEET_METAL);
                box.grid.setMat(i, j, 6, MaterialPalette.SHEET_METAL);
            }
        }
        tools.matA = MaterialPalette.WOOD;
        tools.floodFill(box.grid, 4, 4, 4);
        PaintTools.stretch(box.grid, 2, 2, 2, 3, 3, 3, 2, 2, 2);
        box.grid.assertCubicUnitInvariant();

        tools.sampleFGradient(0x2244AA, 0xFFCC88, 0.35f);
        tools.dropper(box.grid, 4, 4, 4);

        Path modelPath = outDir.resolve("smoke_model.vox.json");
        VoxIO.save(box, modelPath);
        VoxDocument loaded = VoxIO.load(modelPath);
        if (loaded.grid.solidCount() < 1) throw new IllegalStateException("model load empty");
        loaded.grid.assertCubicUnitInvariant();

        // --- sky ---
        VoxDocument sky = new VoxDocument(VoxDocument.Mode.SKY, 28, 1, 14);
        sky.segU = 28;
        sky.segV = 14;
        SkyAndCharacter.paintSkyTiles(sky.grid, sky.segU, sky.segV,
                sky.moonDirX, sky.moonDirY, sky.moonDirZ, sky.moonIntensity);
        Path skyPath = outDir.resolve("smoke_sky.vox.json");
        VoxIO.save(sky, skyPath);
        if (sky.grid.solidCount() < 28) throw new IllegalStateException("sky tiles missing");

        // --- character ---
        VoxDocument character = new VoxDocument(VoxDocument.Mode.CHARACTER, 24, 16, 12);
        character.feetX = 8;
        character.feetY = 0;
        character.feetZ = 4;
        SkyAndCharacter.paintCharacter(character.grid, character.feetX, character.feetY, character.feetZ);
        Path charPath = outDir.resolve("smoke_character.vox.json");
        VoxIO.save(character, charPath);
        if (character.grid.solidCount() < 10) throw new IllegalStateException("character too small");

        // --- bitcrush 32x filter (display only) ---
        int[][] rgbSlice = new int[sky.grid.sizeZ()][sky.grid.sizeX()];
        for (int z = 0; z < sky.grid.sizeZ(); z++)
            for (int x = 0; x < sky.grid.sizeX(); x++)
                rgbSlice[z][x] = sky.grid.getRgb(x, 0, z);
        int[][] crushed = BitcrushUpscale.upscaleAndCrush(rgbSlice, 32, 16);
        if (crushed.length != sky.grid.sizeZ() * 32 || crushed[0].length != sky.grid.sizeX() * 32) {
            throw new IllegalStateException("bitcrush dims wrong");
        }
        // occupancy unchanged
        sky.grid.assertCubicUnitInvariant();

        Path ok = root.resolve("build").resolve("painter").resolve("painter_smoke_ok.txt");
        Files.createDirectories(ok.getParent());
        String report = ""
                + "SMOKE_OK\n"
                + "unit=" + VoxelGrid.UNIT + "\n"
                + "voxel_size=" + VoxelGrid.VOXEL_SIZE + "\n"
                + "model_solids=" + loaded.grid.solidCount() + "\n"
                + "sky_solids=" + sky.grid.solidCount() + "\n"
                + "character_solids=" + character.grid.solidCount() + "\n"
                + "bitcrush_w=" + crushed[0].length + "\n"
                + "bitcrush_h=" + crushed.length + "\n"
                + "cubic_ok=1\n";
        Files.writeString(ok, report);
        System.out.print(report);
    }
}
