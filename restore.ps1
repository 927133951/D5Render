# 一键还原脚本 - 用法: .\restore.ps1 [-Version v215_clean]
# 不带参数时列出所有可用版本
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$bk = Join-Path $root "backups"
$map = @{
    "aim_aim_assist.cpp"     = "aim\aim_assist.cpp"
    "aim_aim_assist.h"       = "aim\aim_assist.h"
    "config_config.h"        = "config\config.h"
    "config_config.cpp"      = "config\config.cpp"
    "gui_app.cpp"            = "gui\app.cpp"
    "utils_update_checker.h" = "utils\update_checker.h"
}
if ($Version -eq "") {
    Write-Host "可用版本快照:"
    Get-ChildItem $bk -Directory | ForEach-Object { Write-Host "  $($_.Name)" }
    Write-Host "用法: .\restore.ps1 -Version <版本名>   (还原后需重新编译)"
    exit 0
}
$dir = Join-Path $bk $Version
if (-not (Test-Path $dir)) { Write-Host "版本不存在: $Version"; exit 1 }
Stop-Process -Name D5Render -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
foreach ($k in $map.Keys) {
    $src = Join-Path $dir $k
    if (Test-Path $src) { Copy-Item $src (Join-Path $root $map[$k]) -Force; Write-Host "已还原 $($map[$k])" }
}
Write-Host "还原完成[$Version], 请重新编译: cmake --build build --config Release --target D5Render"
