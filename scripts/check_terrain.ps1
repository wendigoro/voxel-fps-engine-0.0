#Requires -Version 5.1
<#
.SYNOPSIS
  Terrain gate: every chunk class generates, round-trips, and stays deterministic.
.DESCRIPTION
  Loads a generated terrain map for each of the four chunk classes, exports the
  world it produced, and checks that the exported document regenerates the
  identical grid. Also checks that two different seeds give two different worlds,
  because a generator that ignored its seed would pass every other check here.

  The engine's own --smoke terrain gate (exit 10) covers the height field, the
  road network, the prefab shapes and the fill-only contract. This script covers
  what only a real map load can: the end-to-end path from a file on disk to a
  world, per class.

  Exit 0 all classes pass, 1 otherwise.
#>
param(
  # Seeds used for the determinism pair. Two distinct literals, not a clock.
  [int]$SeedA = 20260930,
  [int]$SeedB = 20260931
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$Exe = Join-Path $Build "voxel_engine.exe"
$Classes = @("valley", "dunes", "tundra", "parkland")

if (-not (Test-Path $Exe)) { throw "voxel_engine.exe missing - run scripts/build.ps1 first" }

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { throw "python not found (needed to write the map fixtures)" }

function Invoke-Engine {
  param([string[]]$EngineArgs)
  $p = Start-Process -FilePath $Exe -ArgumentList $EngineArgs -WorkingDirectory $Build -Wait -PassThru
  return $p.ExitCode
}

$scratch = Join-Path $Build "terrain_gate"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null

$fingerprint = @{}
foreach ($c in $Classes) {
  $src = Join-Path $scratch "$c.map.vox.json"
  $rt1 = Join-Path $scratch "${c}_rt1.map.vox.json"
  $rt2 = Join-Path $scratch "${c}_rt2.map.vox.json"
  & $py.Source (Join-Path $PSScriptRoot "build_terrain_map.py") --out $src --class $c --seed $SeedA | Out-Null
  $code = Invoke-Engine @("--map", $src, "--export-map", $rt1)
  if ($code -ne 0) { throw "${c}: engine failed on --map/--export-map (exit $code)" }
  $code = Invoke-Engine @("--map", $rt1, "--export-map", $rt2)
  if ($code -ne 0) { throw "${c}: engine failed re-loading its own export (exit $code)" }
  $fingerprint[$c] = (Get-FileHash $rt1 -Algorithm SHA256).Hash
}

$ok = $true
foreach ($c in $Classes) {
  $a = Get-Content (Join-Path $scratch "${c}_rt1.map.vox.json") -Raw | ConvertFrom-Json
  $b = Get-Content (Join-Path $scratch "${c}_rt2.map.vox.json") -Raw | ConvertFrom-Json
  $runs = $a.cells_rle.runs.Count
  $same = ($a.cells_rle.runs.Count -eq $b.cells_rle.runs.Count)
  if ($same) {
    for ($i = 0; $i -lt $a.cells_rle.runs.Count; $i++) {
      if ($a.cells_rle.runs[$i] -ne $b.cells_rle.runs[$i]) { $same = $false; break }
    }
  }
  $spawnOk = $null -ne $a.player_spawn -and $a.player_spawn.y -gt 0
  $genOk = $runs -gt 100
  if (-not ($same -and $spawnOk -and $genOk)) { $ok = $false }
  Write-Host ("terrain_gate_{0}: regenerates={1} spawn_y={2} runs={3}" -f `
    $c, $same, $(if ($spawnOk) { $a.player_spawn.y } else { "none" }), $runs)
}

# Distinct classes must not produce distinct-looking-but-equal worlds.
$uniq = ($fingerprint.Values | Select-Object -Unique).Count
if ($uniq -ne $Classes.Count) { $ok = $false }
Write-Host ("terrain_gate_distinct_classes=" + $uniq + "/" + $Classes.Count)

# A generator that ignored its seed would still pass everything above.
# NB: these path variables must not be named $seedA/$seedB -- PowerShell is
# case-insensitive, so they would collide with the [int]$SeedA parameter above.
$mapSeedA = Join-Path $scratch "seedA.map.vox.json"
$mapSeedB = Join-Path $scratch "seedB.map.vox.json"
$outSeedA = Join-Path $scratch "seedA_rt.map.vox.json"
$outSeedB = Join-Path $scratch "seedB_rt.map.vox.json"
& $py.Source (Join-Path $PSScriptRoot "build_terrain_map.py") --out $mapSeedA --class valley --seed $SeedA | Out-Null
& $py.Source (Join-Path $PSScriptRoot "build_terrain_map.py") --out $mapSeedB --class valley --seed $SeedB | Out-Null
if ((Invoke-Engine @("--map", $mapSeedA, "--export-map", $outSeedA)) -ne 0) { throw "seed A load failed" }
if ((Invoke-Engine @("--map", $mapSeedB, "--export-map", $outSeedB)) -ne 0) { throw "seed B load failed" }
$seedSensitive = (Get-FileHash $outSeedA -Algorithm SHA256).Hash -ne
                 (Get-FileHash $outSeedB -Algorithm SHA256).Hash
if (-not $seedSensitive) { $ok = $false }
Write-Host ("terrain_gate_seed_sensitive=" + $seedSensitive)

if (-not $ok) { throw "terrain gate failed" }
Write-Host "TERRAIN_GATE_OK"
exit 0
