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
Copy-Item -LiteralPath (Join-Path $repo 'docs\user-guide.md') -Destination $docsDirectory -Force
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
# The output directory normally sits *inside* the build directory, so a plain recursive scan for
# PDBs finds the ones a previous run already copied into Symbols and copies them in again, one level
# deeper each time. Three runs had produced a 3.3 GB bundle of which 2.1 GB was the bundle itself,
# nested inside its own Symbols folder. Excluding the output path is what stops that.
$outputPrefix = $output.TrimEnd('\') + '\'
foreach ($symbol in (Get-ChildItem -LiteralPath $build -Filter '*.pdb' -File -Recurse |
        Where-Object { $_.FullName -match "\\$Configuration\\" -and
                       -not $_.FullName.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase) })) {
    $relativeSymbol = [Uri]::UnescapeDataString($buildUri.MakeRelativeUri([Uri]$symbol.FullName).ToString()).Replace('/','\')
    $symbolDestination = Join-Path $symbolDirectory $relativeSymbol
    New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($symbolDestination)) | Out-Null
    Copy-Item -LiteralPath $symbol.FullName -Destination $symbolDestination -Force
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
