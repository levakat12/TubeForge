param(
    [Parameter(Mandatory = $true)][string]$Vst3Path,
    [string]$ReportPath = 'host-validation.txt'
)

$ErrorActionPreference = 'Stop'
$plugin = (Resolve-Path -LiteralPath $Vst3Path).Path
$lines = @("TubeForge external host validation", "UTC: $([DateTime]::UtcNow.ToString('o'))", "VST3: $plugin")
$pluginval = $env:TUBEFORGE_PLUGINVAL
if (-not [string]::IsNullOrWhiteSpace($pluginval) -and (Test-Path -LiteralPath $pluginval -PathType Leaf)) {
    & $pluginval --strictness-level 10 --validate $plugin 2>&1 | Tee-Object -Variable pluginvalOutput
    $lines += "pluginval exit: $LASTEXITCODE"
    $lines += $pluginvalOutput
} else {
    $lines += 'pluginval: NOT RUN (set TUBEFORGE_PLUGINVAL)'
}
$lines += 'REAPER manual: record host/version, offline render, automation, state reload, multi-instance, freeze, bypass, latency, mono/stereo, and sample rates.'
$lines += 'VST3PluginTestHost manual: record validator, processing, state, and bus-layout results.'
Set-Content -LiteralPath $ReportPath -Value $lines -Encoding utf8
Write-Output (Resolve-Path -LiteralPath $ReportPath).Path
