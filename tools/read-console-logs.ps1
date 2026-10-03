<#
.SYNOPSIS
Reads SwitchU logs directly from the connected Switch SD card.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\read-console-logs.ps1
#>
[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z]:$')] [string] $ConsoleDrive = 'E:',
    [ValidateRange(1, 5000)] [int] $Tail = 240,
    [switch] $IncludeArchives
)

$ErrorActionPreference = 'Stop'
$logRoot = Join-Path $ConsoleDrive 'config\OmniLaunch'
if (-not (Test-Path -LiteralPath $logRoot)) {
    $legacyLogRoot = Join-Path $ConsoleDrive 'config\SwitchU'
    if (Test-Path -LiteralPath $legacyLogRoot) {
        $logRoot = $legacyLogRoot
    }
}

if (-not (Test-Path -LiteralPath $ConsoleDrive)) {
    throw "Cartao do console $ConsoleDrive nao esta conectado."
}
if (-not (Test-Path -LiteralPath $logRoot)) {
    throw "Nao encontrei os logs do OmniLaunch em $logRoot."
}

$files = @('daemon.log', 'menu.log') |
    ForEach-Object { Join-Path $logRoot $_ } |
    Where-Object { Test-Path -LiteralPath $_ }

if ($IncludeArchives) {
    $files += Get-ChildItem -LiteralPath $logRoot -File -Filter '*.log' |
        Where-Object { $_.Name -notin @('daemon.log', 'menu.log') } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 8 -ExpandProperty FullName
}

if ($files.Count -eq 0) {
    throw "Nenhum log SwitchU encontrado em $logRoot."
}

foreach ($file in $files) {
    $item = Get-Item -LiteralPath $file
    Write-Host "`n### $($item.FullName) ($($item.Length) bytes; $($item.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')))" -ForegroundColor Cyan
    Get-Content -LiteralPath $item.FullName -Tail $Tail
}
