# Builds the plugin, runs the tests and writes release\MW5Motion-v<version><suffix>.zip,
# the file to attach to a GitHub release. Example: .\Package.ps1 -Suffix '-beta'
param([string]$Suffix='')
$ErrorActionPreference='Stop'
& (Join-Path $PSScriptRoot 'Build.ps1')
$cmakeLists=Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'CMakeLists.txt')
if($cmakeLists -notmatch 'project\(MW5Motion VERSION ([0-9.]+)'){throw 'Version not found in CMakeLists.txt.'}
$name="MW5Motion-v$($Matches[1])$Suffix"
$release=Join-Path $PSScriptRoot 'release'
$stage=Join-Path $release $name
if(Test-Path -LiteralPath $stage){Remove-Item -LiteralPath $stage -Recurse -Force}
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'plugins'),(Join-Path $stage 'simhub\profiles') | Out-Null
$dist=Join-Path $PSScriptRoot 'dist'
Copy-Item -LiteralPath (Join-Path $dist 'plugins\MW5Motion.dll') -Destination (Join-Path $stage 'plugins')
Copy-Item -LiteralPath (Join-Path $dist 'mw5_motion.cfg'),(Join-Path $dist 'mech_gait_reference.csv') -Destination $stage
Copy-Item -LiteralPath (Join-Path $dist 'simhub\MechWarrior5.simdef') -Destination (Join-Path $stage 'simhub')
Copy-Item -Path (Join-Path $PSScriptRoot 'simhub\profiles\*.shmotionprofile') -Destination (Join-Path $stage 'simhub\profiles')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md'),(Join-Path $PSScriptRoot 'LICENSE') -Destination $stage
$zip=Join-Path $release "$name.zip"
if(Test-Path -LiteralPath $zip){Remove-Item -LiteralPath $zip -Force}
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Write-Host "Release package: $zip"
