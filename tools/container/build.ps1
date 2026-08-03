# ArmOS
# Copyright (c) 2026 Mohamed Ennassiri
#
# Licensed under the Apache License, Version 2.0.
# See LICENSE for details.
#
# File: tools/container/build.ps1
# Layer: Host tooling / Windows container launcher
#
# Responsibilities:
# - Build ArmOS from native Windows PowerShell through Docker Desktop.
# - Select one explicit repository configuration without shell evaluation.
# - Publish generated kernels and disk images back into the checkout.

[CmdletBinding()]
param(
    [string]$Config = "configs/qemu-virt-arm64.conf",
    [string]$Image = "armos-build:local",
    [switch]$BuildImage,
    [switch]$Qemu,
    [switch]$Rebuild,
    [switch]$Reconfigure
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$Config = $Config.Replace("\", "/")
if ([IO.Path]::IsPathRooted($Config) -or $Config -match '(^|/)\.\.(/|$)') {
    throw "Config must be a path inside the ArmOS repository"
}
$ConfigPath = Join-Path $Root $Config

if (-not (Test-Path -Path $ConfigPath -PathType Leaf)) {
    throw "ArmOS configuration not found: $Config"
}
if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    throw "Docker Desktop is not installed or docker is not in PATH"
}

$ImagePresent = $false
& docker image inspect $Image *> $null
if ($LASTEXITCODE -eq 0) {
    $ImagePresent = $true
}

if ($BuildImage -or -not $ImagePresent) {
    if (-not $ImagePresent) {
        Write-Host "Container image $Image was not found; building it automatically"
    }
    & docker build --pull `
        --file (Join-Path $Root "tools/container/Dockerfile") `
        --tag $Image `
        $Root
    if ($LASTEXITCODE -ne 0) {
        throw "ArmOS container image build failed"
    }
}

$ContainerCommand = @("./build.sh")
if ($Qemu) {
    if ($Rebuild -or $Reconfigure) {
        throw "-Qemu cannot be combined with -Rebuild or -Reconfigure"
    }
    Write-Host "Note: -Qemu produces a Linux QEMU for container/WSL testing."
    Write-Host "A native Windows graphics window requires a native Windows QEMU."
    $ContainerCommand = @("./tools/container/build-qemu.sh")
}

$DockerArguments = @(
    "run", "--rm", "--init",
    "--mount", "type=bind,source=$Root,target=/workspace",
    "--workdir", "/workspace",
    "--env", "ARMOS_CONFIG=$Config",
    $Image
)
$DockerArguments += $ContainerCommand
if (-not $Qemu -and $Rebuild) {
    $DockerArguments += "--rebuild"
} elseif (-not $Qemu -and $Reconfigure) {
    $DockerArguments += "--reconfigure"
}

& docker @DockerArguments
if ($LASTEXITCODE -ne 0) {
    throw "ArmOS container build failed with exit code $LASTEXITCODE"
}

if ($Qemu) {
    Write-Host "Container QEMU is available under $Root\build\host-tools\qemu"
} else {
    Write-Host "ArmOS images are available under $Root\build\images"
}
