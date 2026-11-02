param(
    [string]$Model = 'htdemucs_6s',
    [string]$PythonVersion = '3.12',
    <# Which PyTorch build to install.

       'auto' installs the CUDA build when an NVIDIA GPU is present and the CPU build otherwise.
       This is the single largest lever on how long a song match takes and it used to be left to
       chance: `pip install demucs` resolves torch from PyPI, and **the default torch wheel on
       Windows is CPU-only**, so every installation separated on the processor whatever hardware
       was in the machine. A four-minute song is minutes of every core at full tilt that way, and
       seconds on a GPU.

       'cpu' forces the processor build, which is the right answer on a machine with no NVIDIA
       card or a driver too old for the bundled runtime. #>
    [ValidateSet('auto', 'cpu', 'cuda')]
    [string]$Torch = 'auto'
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

# An NVIDIA GPU is present if the driver's own tool answers. Cheaper and more reliable than
# parsing device lists, and it is the same thing torch will be asking about later.
$hasNvidia = $false
if ($Torch -eq 'cuda') {
    $hasNvidia = $true
} elseif ($Torch -eq 'auto') {
    try {
        $null = & nvidia-smi 2>$null
        $hasNvidia = ($LASTEXITCODE -eq 0)
    } catch {
        $hasNvidia = $false
    }
}

if ($hasNvidia) {
    # Installed *before* demucs so pip resolves torch from this index rather than pulling the
    # CPU wheel from PyPI as a dependency. The CUDA runtime ships inside these wheels, so nothing
    # else has to be installed -- but they do want a reasonably current driver, which is why the
    # failure below falls back rather than throwing.
    Write-Host 'NVIDIA GPU detected - installing the CUDA build of PyTorch.'
    & $python -m pip install --index-url https://download.pytorch.org/whl/cu124 torch torchaudio
    if ($LASTEXITCODE -ne 0) {
        Write-Warning 'CUDA PyTorch install failed; falling back to the CPU build. Song matching will be slower.'
        & $python -m pip install torch torchaudio
        if ($LASTEXITCODE -ne 0) { throw 'PyTorch install failed' }
    }
} else {
    Write-Host 'No NVIDIA GPU detected - installing the CPU build of PyTorch. Song matching will run on the processor.'
}

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
$check = & $python $workerTarget --check --input $workerTarget --output $runtimeRoot
if ($LASTEXITCODE -ne 0) { throw "Demucs runtime self-check failed with exit code $LASTEXITCODE" }
Write-Host "TubeForge ML separator is ready: $manifestPath"

# Said out loud, because it is the difference between a song match taking seconds and taking
# minutes, and the old script left it to be discovered by waiting.
$cudaReady = $false
try { $cudaReady = [bool](($check | ConvertFrom-Json).cudaAvailable) } catch { $cudaReady = $false }
if ($cudaReady) {
    Write-Host 'Separation will run on the GPU.'
} else {
    Write-Warning ('Separation will run on the CPU, which takes minutes per song rather than ' +
                   'seconds. If this machine has an NVIDIA GPU, re-run with -Torch cuda after ' +
                   'updating the graphics driver.')
}
