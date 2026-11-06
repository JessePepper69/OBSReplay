param([ValidateSet('Debug','Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakePath = $cmakeCommand.Source }
else {
    $candidates = @(
        'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe',
        'C:\Program Files\CMake\bin\cmake.exe'
    )
    $cmakePath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $cmakePath) { throw 'Install CMake 3.25+ and Visual Studio C++ desktop tools (MSVC 19.44+).' }
$buildDirectory = Join-Path $PSScriptRoot 'build'
& $cmakePath -S $PSScriptRoot -B $buildDirectory -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& $cmakePath --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
$distributionDirectory = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Force -Path $distributionDirectory | Out-Null
Copy-Item -LiteralPath (Join-Path $buildDirectory "$Configuration\OBSReplay.exe") -Destination (Join-Path $distributionDirectory 'OBSReplay.exe') -Force
Get-Item -LiteralPath (Join-Path $distributionDirectory 'OBSReplay.exe') | Select-Object FullName,Length
