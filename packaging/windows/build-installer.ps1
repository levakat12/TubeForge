param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [string]$OutputDirectory = '',
    [switch]$RequireSigning
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $BuildDirectory 'release' }
$stage = Join-Path $OutputDirectory 'TubeForge-0.10.0-Windows-x64'
& (Join-Path $PSScriptRoot 'stage-release.ps1') -BuildDirectory $BuildDirectory -Configuration $Configuration -OutputDirectory $stage

$certificateSha1 = $env:TUBEFORGE_CERT_SHA1
$signTool = $env:TUBEFORGE_SIGNTOOL
if ($RequireSigning -and ([string]::IsNullOrWhiteSpace($certificateSha1) -or [string]::IsNullOrWhiteSpace($signTool))) {
    throw 'Release signing requires TUBEFORGE_CERT_SHA1 and TUBEFORGE_SIGNTOOL.'
}
if (-not [string]::IsNullOrWhiteSpace($certificateSha1)) {
    if (-not (Test-Path -LiteralPath $signTool -PathType Leaf)) { throw 'TUBEFORGE_SIGNTOOL does not point to signtool.exe.' }
    & $signTool sign /sha1 $certificateSha1 /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 (Join-Path $stage 'Standalone\TubeForge.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Standalone code signing failed.' }
}

$iscc = $env:TUBEFORGE_ISCC
if ([string]::IsNullOrWhiteSpace($iscc)) { $iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe' }
if (-not (Test-Path -LiteralPath $iscc -PathType Leaf)) { throw 'Inno Setup 6 was not found. Set TUBEFORGE_ISCC.' }
$installerOutput = Join-Path $OutputDirectory 'installer'
New-Item -ItemType Directory -Force -Path $installerOutput | Out-Null
& $iscc "/DStageDir=$stage" "/DOutputDir=$installerOutput" (Join-Path $PSScriptRoot 'TubeForge.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed.' }

$installer = Join-Path $installerOutput 'TubeForge-0.10.0-Windows-x64.exe'
if (-not [string]::IsNullOrWhiteSpace($certificateSha1)) {
    & $signTool sign /sha1 $certificateSha1 /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $installer
    if ($LASTEXITCODE -ne 0) { throw 'Installer signing failed.' }
}
if ($RequireSigning -and (Get-AuthenticodeSignature -LiteralPath $installer).Status -ne 'Valid') {
    throw 'Release installer does not have a valid Authenticode signature.'
}
Write-Output $installer
