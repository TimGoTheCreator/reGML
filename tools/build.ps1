param(
    [string]$Compiler = 'clang++',
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$Test,
    [string]$SdlPrefix = '',
    [switch]$NoVideo,
    [switch]$AppOnly
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$outputDir = Join-Path $projectRoot 'build\direct'
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$flags = @('-std=c++20', '-Wall', '-Wextra', '-Wpedantic', '-Wconversion', '-Wsign-conversion', '-I', (Join-Path $projectRoot 'include'))
if ($Configuration -eq 'Debug') { $flags += @('-O0', '-g') }
else { $flags += @('-O2', '-DNDEBUG') }
$coreSources = @('src\core\error.cpp', 'src\formats\package.cpp', 'src\bytecode\decoder.cpp', 'src\bytecode\linker.cpp', 'src\vm\interpreter.cpp', 'src\render\canvas.cpp', 'src\render\assets.cpp', 'src\runtime\demo.cpp', 'src\runtime\game.cpp', 'src\runtime\audio.cpp') |
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
$pngPrefix = Split-Path -Parent (Split-Path -Parent $compilerCommand.Source)
$pngFlags = @('-I', (Join-Path $pngPrefix 'include'), '-L', (Join-Path $pngPrefix 'lib'), '-lpng', '-lvorbisfile')
& $compilerCommand.Source @flags @videoFlags @coreSources (Join-Path $projectRoot 'src\app\main.cpp') (Join-Path $projectRoot 'src\platform\sdl_host.cpp') @videoLibraries @pngFlags '-o' $appExe
if ($LASTEXITCODE -ne 0) { throw "Application compilation failed: $LASTEXITCODE" }
if (-not $NoVideo) { Copy-Item -LiteralPath $sdlDll -Destination (Join-Path $outputDir 'SDL2.dll') -Force }
Copy-Item -LiteralPath (Join-Path $pngPrefix 'bin\libpng16-16.dll') -Destination $outputDir -Force
Copy-Item -LiteralPath (Join-Path $pngPrefix 'bin\zlib1.dll') -Destination $outputDir -Force
foreach ($audioDll in @('libvorbisfile-3.dll', 'libvorbis-0.dll', 'libogg-0.dll')) { Copy-Item -LiteralPath (Join-Path $pngPrefix "bin\$audioDll") -Destination $outputDir -Force }
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
    $playOutput = & $appExe 'play-demo' '--headless' '--frames' '40' '--autoplay' '--screenshot' (Join-Path $outputDir 'arcade.bmp')
    if ($LASTEXITCODE -ne 0 -or ($playOutput -join "`n") -notmatch 'Player: 480, 380') { throw 'Headless gameplay check failed' }
    $playOutput | Write-Output
    if (-not $NoVideo) {
        $previousVideoDriver = $env:SDL_VIDEODRIVER
        $previousRenderDriver = $env:SDL_RENDER_DRIVER
        try {
            $env:SDL_VIDEODRIVER = 'dummy'
            $env:SDL_RENDER_DRIVER = 'software'
            $windowOutput = & $appExe 'play-demo' '--hidden' '--frames' '40' '--autoplay'
            if ($LASTEXITCODE -ne 0) { throw 'SDL presentation check failed' }
            $headlessHash = $playOutput | Where-Object { $_ -like 'Frame hash:*' }
            $windowHash = $windowOutput | Where-Object { $_ -like 'Frame hash:*' }
            if (-not $headlessHash -or $headlessHash -ne $windowHash) { throw 'SDL/headless framebuffer parity failed' }
        } finally {
            $env:SDL_VIDEODRIVER = $previousVideoDriver
            $env:SDL_RENDER_DRIVER = $previousRenderDriver
        }
    }
    Write-Output 'Core tests, CLI checks and gameplay checks passed.'
}
