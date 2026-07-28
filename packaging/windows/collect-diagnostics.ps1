param([string]$Destination = (Join-Path ([Environment]::GetFolderPath('Desktop')) 'TubeForge-Diagnostics.zip'))

$ErrorActionPreference = 'Stop'
$dataRoot = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'TubeForge'
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("TubeForge-Diagnostics-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
foreach ($folder in @('logs','crashes')) {
    $source = Join-Path $dataRoot $folder
    if (Test-Path -LiteralPath $source -PathType Container) { Copy-Item -LiteralPath $source -Destination $temporary -Recurse }
}
@{
    collectedUtc = [DateTime]::UtcNow.ToString('o')
    applicationVersion = '0.10.0'
    operatingSystem = [Environment]::OSVersion.VersionString
    privacy = 'No audio, projects, profile packages, or source file names are included.'
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $temporary 'system.json') -Encoding utf8
Compress-Archive -Path (Join-Path $temporary '*') -DestinationPath $Destination -Force
Remove-Item -LiteralPath $temporary -Recurse -Force
Write-Output $Destination
