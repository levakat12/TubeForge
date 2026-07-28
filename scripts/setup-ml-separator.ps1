param(
    [string]$Model = 'htdemucs_6s',
    [string]$PythonVersion = '3.12'
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$runtimeRoot = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'TubeForge\ml-separation'
$venvRoot = Join-Path $runtimeRoot 'venv'
$workerSource = Join-Path $projectRoot 'ml\scripts\demucs_separator_worker.py'
if (-not (Test-Path -LiteralPath $workerSource)) {
    $workerSource = Join-Path $PSScriptRoot 'demucs_separator_worker.py'
}
$workerTarget = Join-Path $runtimeRoot 'demucs_separator_worker.py'
$manifestPath = Join-Path $runtimeRoot 'runtime.json'

New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
if (-not (Test-Path -LiteralPath $workerSource)) { throw "Worker script is missing: $workerSource" }

$launcher = Join-Path $env:LOCALAPPDATA 'Programs\Python\Launcher\py.exe'
if (-not (Test-Path -LiteralPath $launcher)) { $launcher = 'py.exe' }
if (-not (Test-Path -LiteralPath (Join-Path $venvRoot 'Scripts\python.exe'))) {
    & $launcher "-$PythonVersion" -m venv $venvRoot
}
$python = Join-Path $venvRoot 'Scripts\python.exe'
& $python -m pip install --upgrade pip
& $python -m pip install 'numpy>=2.0,<3' 'demucs>=4.1,<4.2'
Copy-Item -LiteralPath $workerSource -Destination $workerTarget -Force

$runtime = [ordered]@{
    schemaVersion = 1
    python = $python
    worker = $workerTarget
    model = $Model
    device = 'auto'
}
$runtime | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
& $python $workerTarget --check --input $workerTarget --output $runtimeRoot
if ($LASTEXITCODE -ne 0) { throw "Demucs runtime self-check failed with exit code $LASTEXITCODE" }
Write-Host "TubeForge ML separator is ready: $manifestPath"
