<#
.SYNOPSIS
    Deploys unityDeltacast.dll into the Unity project's plugin folder.

.DESCRIPTION
    Checks the built DLL before it goes to Unity:

      * imports: only DLLs that ship with Windows or the VC++ 2015-2022
        redistributable, plus VideoMasterHD*.dll. The VideoMaster DLLs are
        installed into System32 by the VideoMaster driver setup and must match
        the installed driver, so they are never shipped. Anything else (e.g. the
        debug CRT of a Debug build) fails the deployment.
      * exports: every entry point atlasreborn's DeltacastAdapter.cs calls.
        DllImport binds lazily, so a missing export would only fail at runtime.

    The manifest is written to unity-dll-manifest.txt at the repo root. The
    Unity folder is then synced against it: a missing/changed DLL is copied,
    and DLLs (and orphaned .meta files) not in the manifest are deleted.
    Without -Apply the script only reports what it would do.

    Re-run after any rebuild of unityDeltacast. Close the Unity editor first;
    it locks loaded native plugins.

.PARAMETER Apply
    Actually copy/delete in the Unity folder. Default is a dry-run report.

.PARAMETER PluginDir
    Unity plugin folder. Defaults to Assets\Plugins\unityDeltacast of the first
    atlasreborn project found at C:\Projects\atlasReborn_developing or
    D:\Felix\atlasreborn.

.EXAMPLE
    .\tools\deploy_unity.ps1                 # dry-run report
    .\tools\deploy_unity.ps1 -Apply          # sync the Unity folder

.NOTES
    If a machine Group Policy blocks .ps1 execution (MachinePolicy =
    Restricted), run it as a scriptblock from the repo root instead:
      & ([ScriptBlock]::Create((Get-Content -Raw tools\deploy_unity.ps1))) -Apply
#>
[CmdletBinding()]
param(
    # Repo root; auto-detected from the script location, falls back to the
    # current directory when run as a scriptblock (see .NOTES).
    [string]$RepoRoot     = "",
    [string]$BuildDir     = "",
    [string]$PluginDir    = "",
    [string]$ManifestPath = "",
    [switch]$Apply
)

$ErrorActionPreference = "Stop"

if (-not $RepoRoot) {
    if ($PSScriptRoot) { $RepoRoot = Split-Path $PSScriptRoot -Parent }
    else { $RepoRoot = (Get-Location).Path }
}
if (-not $BuildDir)     { $BuildDir     = Join-Path $RepoRoot "build\Release" }
if (-not $ManifestPath) { $ManifestPath = Join-Path $RepoRoot "unity-dll-manifest.txt" }
if (-not $PluginDir) {
    # Aron workstation layout first, then the development checkout.
    foreach ($project in @("C:\Projects\atlasReborn_developing", "D:\Felix\atlasreborn")) {
        if (Test-Path (Join-Path $project "Assets\Plugins")) {
            $PluginDir = Join-Path $project "Assets\Plugins\unityDeltacast"
            break
        }
    }
    if (-not $PluginDir) { throw "No atlasreborn Unity project found - pass -PluginDir." }
}

# Entry points DeltacastAdapter.cs calls (its unused AddInts declaration excluded).
$RequiredExports = @(
    "InitLibrary", "GetMessage", "GetWidth", "GetHeight", "GetFrame",
    "GetNativeFrameCounter", "GetTextureUpdateCallback", "SetBurnInFrameNumber", "GetSignalAndVideoInfo",
    "StartCapture", "StartCaptureStereo", "StopCapture",
    "StartRecording", "StopRecording", "IsRecording", "GetRecordedFrameNumber"
)

# ---------------------------------------------------------------- dumpbin --
function Find-Dumpbin {
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -find "VC\Tools\**\Hostx64\x64\dumpbin.exe" | Select-Object -First 1
        if ($found) { return $found }
    }
    throw "dumpbin.exe not found (need Visual Studio with C++ tools)."
}

# Returns the imported DLL names (static + delay-load) of a PE file.
function Get-ImportedDllNames([string]$DllPath, [string]$Dumpbin) {
    $out = & $Dumpbin /nologo /dependents $DllPath
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $DllPath" }
    $names = @()
    foreach ($line in $out) {
        if ($line -match '^\s+(\S+\.dll)\s*$') { $names += $Matches[1] }
    }
    return $names
}

# Returns the exported function names of a PE file.
function Get-ExportNames([string]$DllPath, [string]$Dumpbin) {
    $out = & $Dumpbin /nologo /exports $DllPath
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed on $DllPath" }
    $names = @()
    foreach ($line in $out) {
        if ($line -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $names += $Matches[1] }
    }
    return $names
}

# ------------------------------------------------------- system DLL filter --
$script:SystemDllPatterns = @(
    '^api-ms-', '^ext-ms-', '^ucrtbase$',
    '^msvcp140(_\d+)?$', '^vcruntime140(_\d+)?$', '^concrt140$', '^vcomp140$',
    '^videomasterhd(_\w+)?$'
)

function Test-IsSystemDll([string]$Name) {
    $base = [IO.Path]::GetFileNameWithoutExtension($Name).ToLowerInvariant()
    foreach ($pat in $script:SystemDllPatterns) {
        if ($base -match $pat) { return $true }
    }
    # Unknown name that ships with Windows itself -> treat as system.
    if (Test-Path (Join-Path "$env:SystemRoot\System32" $Name)) { return $true }
    return $false
}

# ------------------------------------------------------------------ check --
$dumpbin = Find-Dumpbin
Write-Host "dumpbin : $dumpbin"

$mainDll = Join-Path $BuildDir "unityDeltacast.dll"
if (-not (Test-Path $mainDll)) { throw "Not found: $mainDll - build the project first (see README)." }

$foreign = @(Get-ImportedDllNames $mainDll $dumpbin | Where-Object { -not (Test-IsSystemDll $_) })
if ($foreign.Count -gt 0) {
    throw ("unityDeltacast.dll imports DLLs that are not on a target machine: {0} (Debug build?)" -f ($foreign -join ", "))
}

$exports = Get-ExportNames $mainDll $dumpbin
$missing = @($RequiredExports | Where-Object { $exports -notcontains $_ })
if ($missing.Count -gt 0) {
    throw ("unityDeltacast.dll lacks exports DeltacastAdapter.cs calls: {0}" -f ($missing -join ", "))
}
Write-Host ("Checked: imports are system/VC++/VideoMaster only; all {0} required exports present." -f $RequiredExports.Count)

# ---------------------------------------------------------------- manifest --
$item  = Get-Item $mainDll
$entry = [pscustomobject]@{
    Name   = $item.Name
    Bytes  = $item.Length
    Sha256 = (Get-FileHash $mainDll -Algorithm SHA256).Hash
    Source = $mainDll
}

$manifestLines = @("# unity-dll-manifest - generated by tools/deploy_unity.ps1 on $(Get-Date -Format s)")
$manifestLines += "# name`tbytes`tsha256`tsource"
$manifestLines += "{0}`t{1}`t{2}`t{3}" -f $entry.Name, $entry.Bytes, $entry.Sha256, $entry.Source
Set-Content -Path $ManifestPath -Value $manifestLines -Encoding utf8
Write-Host ("Manifest written: {0}  ({1}, {2:N0} bytes)" -f $ManifestPath, $entry.Name, $entry.Bytes)

# ------------------------------------------------------------- Unity sync --
$actions = @()   # @{Op; Name; Src; Dest}
$dest = Join-Path $PluginDir $entry.Name
if (-not (Test-Path $dest)) {
    $actions += @{Op = "COPY-NEW"; Name = $entry.Name; Src = $entry.Source; Dest = $dest}
} else {
    $destItem = Get-Item $dest
    $same = ($destItem.Length -eq $entry.Bytes) -and ((Get-FileHash $dest -Algorithm SHA256).Hash -eq $entry.Sha256)
    $op = "UPDATE"
    if ($same) { $op = "KEEP" }
    $actions += @{Op = $op; Name = $entry.Name; Src = $entry.Source; Dest = $dest}
}

# Extras: DLLs and orphaned .meta files in the plugin folder that are not in the manifest.
if (Test-Path $PluginDir) {
    foreach ($dll in (Get-ChildItem $PluginDir -Filter *.dll -File)) {
        if ($dll.Name -ieq $entry.Name) { continue }
        $actions += @{Op = "DELETE"; Name = $dll.Name; Dest = $dll.FullName}
    }
    foreach ($meta in (Get-ChildItem $PluginDir -Filter *.dll.meta -File)) {
        if ($meta.Name -ieq "$($entry.Name).meta") { continue }
        $actions += @{Op = "DELETE-META"; Name = $meta.Name; Dest = $meta.FullName}
    }
}

# ---------------------------------------------------------------- report --
$mode = "DRY-RUN (use -Apply to execute)"
if ($Apply) { $mode = "APPLY" }
Write-Host ""
Write-Host ("=== Unity sync plan [{0}] -> {1} ===" -f $mode, $PluginDir)
foreach ($a in $actions) {
    Write-Host ("  {0,-11} {1}" -f $a.Op, $a.Dest)
}

if (-not $Apply) { return }

# ----------------------------------------------------------------- apply --
if (-not (Test-Path $PluginDir)) { New-Item -ItemType Directory -Path $PluginDir | Out-Null }
foreach ($a in $actions) {
    switch ($a.Op) {
        "COPY-NEW"    { Copy-Item $a.Src $a.Dest -Force }
        "UPDATE"      { Copy-Item $a.Src $a.Dest -Force }
        "DELETE"      { Remove-Item $a.Dest -Force }
        "DELETE-META" { Remove-Item $a.Dest -Force }
    }
}
Write-Host "Applied. Note: a newly copied DLL gets its .meta generated on the next Unity editor open - commit that too."
