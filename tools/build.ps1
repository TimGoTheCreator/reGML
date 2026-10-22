param(
    [string]$Compiler = 'clang++',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$Test,
    [string]$SdlPrefix = '',
    [string]$DependencyPrefix = '',
    [switch]$NoVideo,
    [switch]$AppOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\direct'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wsign-conversion', '-I', (Join-Path $projectRoot 'src'))
if ($Configuration -eq 'Debug') { $flags += @('-O0', '-g') }
else { $flags += @('-O2', '-DNDEBUG') }
$coreSources = @('src\error.cpp', 'src\package.cpp', 'src\decoder.cpp', 'src\linker.cpp', 'src\interpreter.cpp', 'src\rendersoftware\canvas.cpp', 'src\rendersoftware\assets.cpp', 'src\demo\demo.cpp', 'src\game.cpp', 'src\audio\sdl\audio.cpp') |
    ForEach-Object { Join-Path $projectRoot $_ }
$appExe = Join-Path $outputDir 'regml.exe'
$testExe = Join-Path $outputDir 'regml_tests.exe'
$videoFlags = @('-DREGML_WITH_SDL2=0')
$videoLibraries = @()
if (-not $NoVideo) {
    if ($SdlPrefix -eq '') { $SdlPrefix = Split-Path -Parent (Split-Path -Parent $compilerCommand.Source) }
    $sdlHeader = Join-Path $SdlPrefix 'include\SDL2\SDL.h'
    $sdlDll = Join-Path $SdlPrefix 'bin\SDL2.dll'
    if (-not (Test-Path -LiteralPath $sdlHeader) -or -not (Test-Path -LiteralPath $sdlDll)) {
        throw 'SDL2 is not available at the compiler prefix. Supply -SdlPrefix or use -NoVideo for a headless build.'
    }
    $videoFlags = @('-DREGML_WITH_SDL2=1', '-DSDL_MAIN_HANDLED', '-I', (Join-Path $SdlPrefix 'include'))
    $videoLibraries = @('-L', (Join-Path $SdlPrefix 'lib'), '-lSDL2')
}
if ($DependencyPrefix -eq '') { $DependencyPrefix = Split-Path -Parent (Split-Path -Parent $compilerCommand.Source) }
$pngPrefix = $DependencyPrefix
function Find-DependencyLibrary([string[]]$Names) {
    foreach ($name in $Names) {
        $candidate = Join-Path $DependencyPrefix "lib\$name"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw "Missing dependency library in $DependencyPrefix\lib: $($Names -join ', ')"
}
$pngFlags = @('-I', (Join-Path $pngPrefix 'include'),
    (Find-DependencyLibrary @('libpng16.lib', 'png.lib', 'libpng.dll.a', 'libpng.a')),
    (Find-DependencyLibrary @('vorbisfile.lib', 'libvorbisfile.dll.a', 'libvorbisfile.a')),
    (Find-DependencyLibrary @('vorbis.lib', 'libvorbis.dll.a', 'libvorbis.a')),
    (Find-DependencyLibrary @('ogg.lib', 'libogg.dll.a', 'libogg.a')),
    (Find-DependencyLibrary @('z.lib', 'zs.lib', 'zlib.lib', 'zlib1.lib', 'zlibstatic.lib', 'libz.dll.a', 'libz.a')))
& $compilerCommand.Source @flags @videoFlags @coreSources (Join-Path $projectRoot 'src\main.cpp') (Join-Path $projectRoot 'src\platform\sdl\sdl.cpp') (Join-Path $projectRoot 'src\platform\wii\host.cpp') (Join-Path $projectRoot 'src\platform\wii\renderer.cpp') @videoLibraries @pngFlags '-o' $appExe
if ($LASTEXITCODE -ne 0) { throw "Application compilation failed: $LASTEXITCODE" }
if (-not $NoVideo) { Copy-Item -LiteralPath $sdlDll -Destination (Join-Path $outputDir 'SDL2.dll') -Force }
Get-ChildItem -LiteralPath (Join-Path $pngPrefix 'bin') -Filter '*.dll' -File |
    Where-Object { $_.Name -match '^(lib)?(png\d*|vorbisfile|vorbis|ogg|zlib\d*|z)(-\d+)?\.dll$' } |
    Copy-Item -Destination $outputDir -Force
if (-not $AppOnly -or $Test) {
    & $compilerCommand.Source @flags @coreSources (Join-Path $projectRoot 'tests\core_tests.cpp') @pngFlags '-o' $testExe
    if ($LASTEXITCODE -ne 0) { throw "Test compilation failed: $LASTEXITCODE" }
}
Write-Output "Built reGML ($Configuration): $appExe"
if ($Test) {
    & $testExe
    if ($LASTEXITCODE -ne 0) { throw "Core tests failed: $LASTEXITCODE" }
    & $appExe '--help'
    if ($LASTEXITCODE -ne 0) { throw 'CLI help failed' }
    $demoOutput = & $appExe 'vm-demo' '--trace'
    if ($LASTEXITCODE -ne 0 -or ($demoOutput -join "`n") -notmatch 'Result: 42') { throw 'CLI numeric demo failed' }
    $demoOutput | Write-Output
    $fixturePath = Join-Path $outputDir 'synthetic-data.win'
    & $testExe '--emit-fixture' $fixturePath
    if ($LASTEXITCODE -ne 0) { throw 'Fixture creation failed' }
    $inspection = & $appExe 'inspect' $fixturePath '--code' '--strings'
    if ($LASTEXITCODE -ne 0 -or ($inspection -join "`n") -notmatch 'gml_Script_test') { throw 'CLI package inspection failed' }
    $inspection | Write-Output
    & $appExe 'disasm' $fixturePath '0'
    if ($LASTEXITCODE -ne 0) { throw 'CLI disassembly failed' }
    & $appExe 'audit' $fixturePath
    if ($LASTEXITCODE -ne 0) { throw 'CLI symbol audit failed' }
    & $appExe 'disasm' $fixturePath '0' '--linked'
    if ($LASTEXITCODE -ne 0) { throw 'CLI linked disassembly failed' }
    & $appExe 'unknown-command'
    if ($LASTEXITCODE -ne 2) { throw 'CLI usage exit code is incorrect' }
    Write-Output 'Core tests and CLI checks passed.'
}
