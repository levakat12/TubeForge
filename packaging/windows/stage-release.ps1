param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$output = [IO.Path]::GetFullPath($OutputDirectory)
$standalone = Join-Path $build "nts_standalone_app_artefacts\$Configuration\TubeForge.exe"
$vst3 = Join-Path $build "TubeForge_artefacts\$Configuration\VST3\TubeForge.vst3"
if (-not (Test-Path -LiteralPath $standalone -PathType Leaf)) { throw "Standalone artifact is missing: $standalone" }
if (-not (Test-Path -LiteralPath $vst3 -PathType Container)) { throw "VST3 artifact is missing: $vst3" }

New-Item -ItemType Directory -Force -Path $output | Out-Null
$appDirectory = Join-Path $output 'Standalone'
$pluginDirectory = Join-Path $output 'VST3'
$docsDirectory = Join-Path $output 'Documentation'
$toolsDirectory = Join-Path $output 'Tools'
New-Item -ItemType Directory -Force -Path $appDirectory,$pluginDirectory,$docsDirectory,$toolsDirectory | Out-Null
Copy-Item -LiteralPath $standalone -Destination $appDirectory -Force
Copy-Item -LiteralPath $vst3 -Destination $pluginDirectory -Recurse -Force

$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
foreach ($document in @('README.md','PRIVACY.md','SECURITY.md','THIRD_PARTY_NOTICES.md')) {
    $source = Join-Path $repo $document
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $docsDirectory -Force }
}
Copy-Item -LiteralPath (Join-Path $repo 'USER_GUIDE.md') -Destination $docsDirectory -Force
Copy-Item -LiteralPath (Join-Path $repo 'packaging\windows\collect-diagnostics.ps1') -Destination $output -Force
Copy-Item -LiteralPath (Join-Path $repo 'scripts\setup-ml-separator.ps1') -Destination $toolsDirectory -Force
Copy-Item -LiteralPath (Join-Path $repo 'ml\scripts\demucs_separator_worker.py') -Destination $toolsDirectory -Force

$symbolDirectory = Join-Path $output 'Symbols'
# Cleared first, so a bundle only ever contains the symbols of the build that produced it. Left in
# place, PDBs from deleted targets and from earlier versions accumulated -- the bundle staged today
# still carried files dated three weeks back.
if (Test-Path -LiteralPath $symbolDirectory) { Remove-Item -LiteralPath $symbolDirectory -Recurse -Force }
New-Item -ItemType Directory -Force -Path $symbolDirectory | Out-Null
$buildUri = [Uri]($build.TrimEnd('\') + '\')
# Only the symbols for the two binaries this bundle actually ships.
#
# This used to be a recursive scan of the whole build tree for PDBs, and the result was a 1.4 GB
# Symbols folder against 30 MB of shipped binaries. What filled it was test and benchmark
# executables -- 157 MB of nts_wrapper_tests, 91 MB of nts_plugin_benchmarks, 78 MB of
# nts_vst3_host_tests -- none of which is in the bundle and none of which anybody can symbolicate a
# crash report against, because users never run them.
#
# Worse on a developer machine than on CI: an incremental build directory keeps object files for
# targets that no longer exist, so the scan happily staged nts_circuit.pdb and
# nts_circuit_tests.pdb for an engine that had been deleted from the tree. Clearing the destination
# above cannot fix that -- the staleness is in the source.
#
# Named explicitly rather than filtered, so adding a shipped binary is a deliberate edit here and a
# new test target cannot quietly add a hundred megabytes to a release.
$shippedSymbols = @(
    (Join-Path $build "TubeForge_artefacts\$Configuration\VST3\TubeForge.pdb"),
    (Join-Path $build "nts_standalone_app_artefacts\$Configuration\TubeForge.pdb")
)
foreach ($symbol in $shippedSymbols) {
    # A bundle whose symbols are missing is worse than one with none, because it looks complete and
    # then cannot symbolicate the one crash it was kept for. Fail rather than ship that.
    if (-not (Test-Path -LiteralPath $symbol -PathType Leaf)) { throw "Shipped symbol is missing: $symbol" }
    $relativeSymbol = [Uri]::UnescapeDataString($buildUri.MakeRelativeUri([Uri]$symbol).ToString()).Replace('/','\')
    $symbolDestination = Join-Path $symbolDirectory $relativeSymbol
    New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($symbolDestination)) | Out-Null
    Copy-Item -LiteralPath $symbol -Destination $symbolDestination -Force
}

$baseUri = [Uri]($output.TrimEnd('\') + '\')
$hashes = Get-ChildItem -LiteralPath $output -File -Recurse |
    Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
    Sort-Object FullName |
    ForEach-Object {
        $relative = [Uri]::UnescapeDataString($baseUri.MakeRelativeUri([Uri]$_.FullName).ToString())
        "{0}  {1}" -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
    }
Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Value $hashes -Encoding utf8
Write-Output $output
