# Compile and launch the Voxel Painter Swing UI (JDK 25+).
param(
    [switch]$CompileOnly,
    [switch]$HeadlessSmoke
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not (Test-Path (Join-Path $RepoRoot "java\painter\src"))) {
    $RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
}

$SrcRoot = Join-Path $RepoRoot "java\painter\src"
$OutDir  = Join-Path $RepoRoot "java\painter\out"
$Main    = "voxel.painter.ui.PainterApp"

Write-Host "RepoRoot = $RepoRoot"
& javac -version
& java -version
if (-not (Test-Path $SrcRoot)) { throw "Missing sources at $SrcRoot" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$sources = Get-ChildItem -Path $SrcRoot -Recurse -Filter *.java | ForEach-Object { $_.FullName }
if (-not $sources -or $sources.Count -eq 0) { throw "No .java files under $SrcRoot" }

Write-Host "Compiling $($sources.Count) sources -> $OutDir"
& javac --release 25 -encoding UTF-8 -d $OutDir $sources
if ($LASTEXITCODE -ne 0) { throw "javac failed with exit $LASTEXITCODE" }
Write-Host "Compile OK."

if ($HeadlessSmoke) {
    Write-Host "Headless smoke against real grid API"
    $smokeJava = @"
import java.nio.file.*;
import voxel.painter.grid.*;
import voxel.painter.filter.BitcrushUpscale;
public class PainterUiSmoke {
  public static void main(String[] a) throws Exception {
    if (Math.abs(VoxelGrid.VOXEL_SIZE - 0.001f) > 1e-8f) throw new IllegalStateException("VOXEL_SIZE");
    VoxDocument doc = new VoxDocument(VoxDocument.Mode.MODEL, 16, 16, 16);
    doc.grid.assertCubicUnitInvariant();
    PaintTools tools = new PaintTools();
    tools.shape = PaintTools.BrushShape.CUBE;
    tools.brushSize = 3;
    tools.matA = MaterialPalette.WOOD;
    tools.rgbA = MaterialPalette.defaultRgb(MaterialPalette.WOOD);
    tools.paintAt(doc.grid, 8, 8, 8);
    tools.sampleFGradient(0x112233, 0xAABBCC, 0.5f);
    Path p = Files.createTempFile("painter-ui-", ".vox.json");
    VoxIO.save(doc, p);
    VoxDocument loaded = VoxIO.load(p);
    if (loaded.grid.solidCount() < 1) throw new IllegalStateException("empty load");
    if (loaded.unit != 1) throw new IllegalStateException("unit");
    VoxDocument sky = new VoxDocument(VoxDocument.Mode.SKY, 28, 1, 14);
    SkyAndCharacter.paintSkyTiles(sky.grid, sky.segU, sky.segV, sky.moonDirX, sky.moonDirY, sky.moonDirZ, sky.moonIntensity);
    int[][] src = new int[1][2];
    src[0][0] = 0x2244AA; src[0][1] = 0xFFCC88;
    int[][] out = BitcrushUpscale.upscaleAndCrush(src, 32, 16);
    if (out.length != 32 || out[0].length != 64) throw new IllegalStateException("bitcrush dims");
    System.out.println("SMOKE_OK mode=" + loaded.modeName()
      + " solids=" + loaded.grid.solidCount()
      + " sky_solids=" + sky.grid.solidCount()
      + " bitcrush=" + out[0].length + "x" + out.length
      + " VOXEL_SIZE=" + VoxelGrid.VOXEL_SIZE);
    Files.deleteIfExists(p);
  }
}
"@
    $smokeSrcDir = Join-Path $OutDir "_smoke_src"
    New-Item -ItemType Directory -Force -Path $smokeSrcDir | Out-Null
    $smokeFile = Join-Path $smokeSrcDir "PainterUiSmoke.java"
    $utf8NoBom = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($smokeFile, $smokeJava, $utf8NoBom)
    & javac --release 25 -encoding UTF-8 -cp $OutDir -d $OutDir $smokeFile
    if ($LASTEXITCODE -ne 0) { throw "smoke javac failed" }
    & java -cp $OutDir PainterUiSmoke
    if ($LASTEXITCODE -ne 0) { throw "smoke failed" }
    Write-Host "Headless smoke passed."
    if ($CompileOnly) { exit 0 }
}

if ($CompileOnly) {
    Write-Host "CompileOnly set; not launching UI."
    exit 0
}

Write-Host "Launching $Main ..."
& java -cp $OutDir $Main $RepoRoot
exit $LASTEXITCODE
