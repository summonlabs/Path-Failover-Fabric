# Fresh clone closure: build, test, install and consume the released sources.
#
# The script never writes into the source checkout: every build, staging and
# consumer directory is created under the work directory it is given, so it is
# safe to run against a read-only clone.
#
# Usage:
#   pwsh -File scripts/fresh-clone-closure.ps1 -SourceDir <clone> -WorkDir <scratch>
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$SourceDir,
  [Parameter(Mandatory = $true)][string]$WorkDir,
  [string]$Configuration = "Release",
  [string]$Generator = "Ninja"
)

$ErrorActionPreference = "Stop"

function Write-Step([string]$Text) {
  Write-Host ""
  Write-Host "=== $Text ==="
}

$source = (Resolve-Path -LiteralPath $SourceDir).Path
if (-not (Test-Path -LiteralPath $WorkDir)) {
  New-Item -ItemType Directory -Path $WorkDir | Out-Null
}
$work = (Resolve-Path -LiteralPath $WorkDir).Path

$build = Join-Path $work "build"
$prefix = Join-Path $work "install"
$consumerBuild = Join-Path $work "consumer-build"

Write-Step "Configure ($Configuration)"
$configure = @("-S", $source, "-B", $build, "-G", $Generator,
               "-DCMAKE_BUILD_TYPE=$Configuration",
               "-DCMAKE_INSTALL_PREFIX=$prefix",
               "-DPFF_BUILD_TESTS=ON", "-DPFF_BUILD_TOOLS=ON", "-DPFF_BUILD_EXAMPLES=ON")
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

Write-Step "Build"
& cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Step "Test"
Push-Location $build
try {
  & ctest --output-on-failure
  if ($LASTEXITCODE -ne 0) { throw "ctest failed" }
} finally {
  Pop-Location
}

Write-Step "Install"
& cmake --install $build
if ($LASTEXITCODE -ne 0) { throw "install failed" }

Write-Step "Build the independent consumer against the installed prefix"
$consumerConfigure = @("-S", (Join-Path $source "examples/consumer"), "-B", $consumerBuild,
                       "-G", $Generator, "-DCMAKE_BUILD_TYPE=$Configuration",
                       "-DCMAKE_PREFIX_PATH=$prefix")
& cmake @consumerConfigure
if ($LASTEXITCODE -ne 0) { throw "consumer configure failed" }
& cmake --build $consumerBuild
if ($LASTEXITCODE -ne 0) { throw "consumer build failed" }

Write-Step "Run the consumer"
& (Join-Path $consumerBuild "pff_consumer.exe")
if ($LASTEXITCODE -ne 0) { throw "consumer run failed" }

Write-Step "Run the example against the built artifacts"
& (Join-Path $build "pff_example_quickstart.exe") | Select-Object -First 6
if ($LASTEXITCODE -ne 0) { throw "example failed" }

Write-Step "Closure OK"
Write-Host "source   : $source"
Write-Host "build    : $build"
Write-Host "prefix   : $prefix"
Write-Host "consumer : $consumerBuild"
