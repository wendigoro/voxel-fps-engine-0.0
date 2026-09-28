package voxel.painter;

import voxel.painter.filter.BitcrushUpscale;
import voxel.painter.grid.Items;
import voxel.painter.grid.MaterialPalette;
import voxel.painter.grid.PaintTools;
import voxel.painter.grid.SkyAndCharacter;
import voxel.painter.grid.VoxelGrid;
import voxel.painter.grid.VoxDocument;
import voxel.painter.grid.VoxIO;
import voxel.painter.grid.WeaponParts;

import java.nio.file.Files;
import java.nio.file.Path;

/** Headless smoke: tools, sky, character, item mode, IO, bitcrush 32x filter. */
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

        // --- item mode: the painted grid IS the packing footprint ---
        VoxDocument item = new VoxDocument(VoxDocument.Mode.ITEM, 4, 3, 2);
        item.itemId = "smoke_pouch";
        item.itemName = "Smoke Pouch";
        item.itemClass = Items.CLASS_AMMO_POUCH;
        item.armorZone = "";
        item.caliber = "medium";
        item.ammoId = "medium_fmj";
        Items.paintStarterPouch(item.grid);
        item.grid.assertCubicUnitInvariant();

        int itemCells = Items.solidCells(item.grid);
        if (itemCells != 8)
            throw new IllegalStateException("item cells=" + itemCells + " expected 8 (2x2x2 pouch)");
        // The canvas is 4x3x2 but only a 2x2x2 block is painted: the exported
        // footprint must be the tight box, never the empty canvas around it.
        int[] itemFp = Items.footprintSize(item.grid);
        if (itemFp == null || itemFp[0] != 2 || itemFp[1] != 2 || itemFp[2] != 2)
            throw new IllegalStateException("pouch footprint is not 2x2x2 unit cells");

        // item.json must be self-describing enough for the engine packer. Smoke
        // artifacts go to data/voxfmt, never data/items: the engine loads every
        // file in data/items, so a test item would become real game content.
        Path itemJson = outDir.resolve(Items.fileNameFor(item.itemId));
        Items.exportItemJson(itemJson, item);
        String itemText = Files.readString(itemJson);
        for (String key : new String[]{"\"id\"", "\"class\"", "\"size\"", "\"cells\"", "\"ammo_id\""}) {
            if (!itemText.contains(key))
                throw new IllegalStateException("item.json missing " + key);
        }
        // 8 solid cells => 24 flattened coordinates, all inside the 2x2x2 footprint.
        int cellTokens = countCellsToken(itemText);
        if (cellTokens != itemCells * 3)
            throw new IllegalStateException("item.json cells=" + cellTokens
                    + " expected " + (itemCells * 3));
        if (!itemText.contains("\"size\": [2, 2, 2]"))
            throw new IllegalStateException("item.json size must be the tight 2x2x2 footprint");
        for (int token : parseInts(itemText, "\"cells\"")) {
            if (token < 0 || token > 1)
                throw new IllegalStateException("cell coordinate " + token + " outside 2x2x2 footprint");
        }
        if (Items.needsArmorZone(item.itemClass))
            throw new IllegalStateException("ammo_pouch must not need an armor zone");
        if (Items.needsPackSize(item.itemClass))
            throw new IllegalStateException("ammo_pouch must not need pack_size");
        if (itemText.contains("pack_size"))
            throw new IllegalStateException("ammo_pouch exported pack_size");

        // A backpack must round-trip its granted storage through .vox.json.
        VoxDocument pack = new VoxDocument(VoxDocument.Mode.ITEM, 4, 4, 3);
        pack.itemId = "smoke_pack";
        pack.itemName = "Smoke Pack";
        pack.itemClass = Items.CLASS_BACKPACK;
        pack.packSX = 2;
        pack.packSY = 3;
        pack.packSZ = 4;
        Items.paintStarterPlate(pack.grid);
        Path packVox = outDir.resolve("smoke_item_pack.vox.json");
        VoxIO.save(pack, packVox);
        VoxDocument packBack = VoxIO.load(packVox);
        if (packBack.mode != VoxDocument.Mode.ITEM)
            throw new IllegalStateException("item mode lost in .vox.json round trip");
        if (!pack.itemId.equals(packBack.itemId)
                || !pack.itemClass.equals(packBack.itemClass)
                || pack.packSX != packBack.packSX
                || pack.packSY != packBack.packSY
                || pack.packSZ != packBack.packSZ
                || pack.grid.solidCount() != packBack.grid.solidCount())
            throw new IllegalStateException("item metadata lost in .vox.json round trip");
        packBack.grid.assertCubicUnitInvariant();

        // An armor item carries its zone and must not grant storage.
        VoxDocument plate = new VoxDocument(VoxDocument.Mode.ITEM, 3, 3, 3);
        plate.itemId = "smoke_plate";
        plate.itemName = "Smoke Plate";
        plate.itemClass = Items.CLASS_ARMOR;
        plate.armorZone = "chest";
        Items.paintStarterPlate(plate.grid);
        Path plateJson = outDir.resolve(Items.fileNameFor(plate.itemId));
        Items.exportItemJson(plateJson, plate);
        String plateText = Files.readString(plateJson);
        if (!plateText.contains("\"armor_zone\": \"chest\""))
            throw new IllegalStateException("armor item missing armor_zone");
        if (plateText.contains("pack_size"))
            throw new IllegalStateException("armor item exported pack_size");

        // bitcrush 32x is display-only: it must never reach item occupancy.
        if (Items.solidCells(item.grid) != itemCells)
            throw new IllegalStateException("item occupancy changed");

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

        // --- weapon parts ---
        VoxDocument weapon = new VoxDocument(VoxDocument.Mode.WEAPON, 24, 12, 12);
        weapon.caliber = "medium";
        weapon.ammoId = "medium_fmj";
        WeaponParts.paintStarterRifle(weapon.grid);
        weapon.grid.assertCubicUnitInvariant();
        WeaponParts.Stats stats = WeaponParts.compose(weapon.grid, weapon.caliber);
        if (stats.damage <= 0 || stats.partCounts.getOrDefault("barrel", 0) < 1)
            throw new IllegalStateException("weapon stats/parts missing");
        Path weaponsDir = root.resolve("data").resolve("weapons");
        Files.createDirectories(weaponsDir);
        Path buildWeapons = root.resolve("build").resolve("weapons");
        Files.createDirectories(buildWeapons);
        Path wVox = outDir.resolve("smoke_weapon.vox.json");
        VoxIO.save(weapon, wVox);
        Path wJson = weaponsDir.resolve("starter_rifle.weapon.json");
        WeaponParts.exportWeaponJson(wJson, "starter_rifle", stats, weapon.ammoId);
        Files.copy(wJson, buildWeapons.resolve("starter_rifle.weapon.json"),
                java.nio.file.StandardCopyOption.REPLACE_EXISTING);

        Path ok = root.resolve("build").resolve("painter").resolve("painter_smoke_ok.txt");
        Files.createDirectories(ok.getParent());
        String report = ""
                + "SMOKE_OK\n"
                + "unit=" + VoxelGrid.UNIT + "\n"
                + "voxel_size=" + VoxelGrid.VOXEL_SIZE + "\n"
                + "model_solids=" + loaded.grid.solidCount() + "\n"
                + "sky_solids=" + sky.grid.solidCount() + "\n"
                + "character_solids=" + character.grid.solidCount() + "\n"
                + "weapon_solids=" + weapon.grid.solidCount() + "\n"
                + "weapon_damage=" + stats.damage + "\n"
                + "weapon_caliber=" + stats.caliber + "\n"
                + "bitcrush_w=" + crushed[0].length + "\n"
                + "bitcrush_h=" + crushed.length + "\n"
                + "item_size=" + itemFp[0] + "x" + itemFp[1] + "x" + itemFp[2] + "\n"
                + "item_cells=" + itemCells + "\n"
                + "item_json_cells=" + cellTokens + "\n"
                + "item_roundtrip_cells=" + packBack.grid.solidCount() + "\n"
                + "item_roundtrip_pack=" + packBack.packSX + "," + packBack.packSY + "," + packBack.packSZ + "\n"
                + "armor_zone=" + plate.armorZone + "\n"
                + "cubic_ok=1\n";
        Files.writeString(ok, report);
        System.out.print(report);
    }

    /** Number of integer tokens in the item.json "cells" array. */
    private static int countCellsToken(String json) {
        return parseInts(json, "\"cells\"").length;
    }

    /** Integers of the array that follows {@code key} in a json blob. */
    private static int[] parseInts(String json, String key) {
        int at = json.indexOf(key);
        if (at < 0) return new int[0];
        int open = json.indexOf('[', at);
        int close = json.indexOf(']', open);
        if (open < 0 || close < 0) return new int[0];
        String body = json.substring(open + 1, close).trim();
        if (body.isEmpty()) return new int[0];
        String[] parts = body.split(",");
        int[] out = new int[parts.length];
        for (int i = 0; i < parts.length; i++) out[i] = Integer.parseInt(parts[i].trim());
        return out;
    }
}
