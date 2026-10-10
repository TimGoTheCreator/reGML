param([Parameter(Mandatory=$true)][string]$DataFile)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $projectRoot
$runnerPath = Join-Path $projectRoot 'build\direct\regml.exe'
$inputPath = Join-Path $projectRoot 'tests\fixtures\undertale-flowey.txt'
if (-not (Test-Path -LiteralPath $DataFile)) { throw 'Supply -DataFile with your installed Undertale data.win.' }
$artifactDir = Join-Path $projectRoot ('build\undertale-check-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $artifactDir | Out-Null
$commonArgs = @('run', $DataFile, '--frames', '3500', '--input-script', $inputPath, '--seed', '1')
$headless = & $runnerPath @commonArgs '--headless' '--save-dir' (Join-Path $artifactDir 'headless-saves') '--screenshot' (Join-Path $artifactDir 'headless.bmp')
if ($LASTEXITCODE -ne 0 -or ($headless -join "`n") -notmatch 'Room: room_floweybattle') { throw 'Headless progression did not reach Flowey battle.' }
$native = & $runnerPath @commonArgs '--hidden' '--save-dir' (Join-Path $artifactDir 'native-saves') '--screenshot' (Join-Path $artifactDir 'native.bmp')
if ($LASTEXITCODE -ne 0 -or ($native -join "`n") -notmatch 'Room: room_floweybattle') { throw 'SDL progression did not reach Flowey battle.' }
$headlessHash = $headless | Where-Object { $_ -like 'Frame hash:*' }
$nativeHash = $native | Where-Object { $_ -like 'Frame hash:*' }
if (-not $headlessHash -or $headlessHash -ne $nativeHash) { throw "Framebuffer mismatch: $headlessHash / $nativeHash" }
if ((Get-FileHash -LiteralPath (Join-Path $artifactDir 'headless.bmp')).Hash -ne (Get-FileHash -LiteralPath (Join-Path $artifactDir 'native.bmp')).Hash) { throw 'Screenshot bytes differ.' }
$headless | Write-Output
Write-Output "PASS Undertale progression and SDL framebuffer parity. Artifacts: $artifactDir"
$testRunner = Join-Path $projectRoot 'build\direct\regml_tests.exe'
& $testRunner '--flowey-regression' $DataFile $inputPath (Join-Path $artifactDir 'flowey-regression')
if ($LASTEXITCODE -ne 0) { throw 'Flowey pellet/dodge/hit regression failed.' }
