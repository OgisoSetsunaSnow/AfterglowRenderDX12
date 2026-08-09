# ============================================================
# AfterglowRender DX12 - Release packaging script
# Usage: powershell -ExecutionPolicy Bypass -File make_release.ps1
# Output: AfterglowRenderDX12_Release.zip (run exe directly, no VS needed)
# ============================================================
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\32991\Desktop\AfterglowRenderForDX12\DX12'
Set-Location $root

Write-Host '[1/6] Building Release...'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Enterprise\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild 'AfterglowRenderDX12.vcxproj' -p:Configuration=Release -p:Platform=x64 -m -v:m
if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }

$pkg = Join-Path $root 'Release_Package'
$pkgRoot = Join-Path $pkg 'AfterglowRenderDX12'
if (Test-Path $pkg) { Remove-Item $pkg -Recurse -Force }
New-Item -ItemType Directory -Force -Path $pkgRoot | Out-Null

Write-Host '[2/6] Copying exe + runtime DLLs...'
Copy-Item "$root\x64\Release\AfterglowRenderDX12.exe" $pkgRoot
Get-ChildItem "$root\x64\Release\*.dll" | Copy-Item -Destination $pkgRoot

Write-Host '[3/6] Copying Shaders / Textures...'
Copy-Item "$root\Shaders" $pkgRoot -Recurse
Copy-Item "$root\Textures" $pkgRoot -Recurse

Write-Host '[4/6] Copying Assets (excluding local music files)...'
New-Item -ItemType Directory -Force -Path "$pkgRoot\vsbuild\Assets" | Out-Null
Copy-Item "$root\vsbuild\Assets\Characters" "$pkgRoot\vsbuild\Assets\" -Recurse
Copy-Item "$root\vsbuild\Assets\Shared" "$pkgRoot\vsbuild\Assets\" -Recurse
New-Item -ItemType Directory -Force -Path "$pkgRoot\vsbuild\Assets\Music" | Out-Null
if (Test-Path "$root\vsbuild\Assets\Music\README.txt") {
    Copy-Item "$root\vsbuild\Assets\Music\README.txt" "$pkgRoot\vsbuild\Assets\Music\"
}

Write-Host '[5/6] Writing README...'
$readmeLines = @(
    'AfterglowRender DX12 - D3D12 render demo (ported from Vulkan)',
    '=================================================',
    'Run AfterglowRenderDX12.exe directly (no VS / SDK required).',
    '',
    'Controls:',
    '  Left mouse drag   orbit camera rotate',
    '  Middle drag       pan target',
    '  Scroll            zoom',
    '  Right mouse drag  free camera look',
    '  W/A/S/D, E/Q      free camera move',
    '  F                 toggle orbit/free camera',
    '  ~ (backtick)      toggle ImGui console (postfx / camera / scene / audio)',
    '  1/2/3             chromatic aberration / dust / vignette',
    '  5/6/7/8           debug views (noise / scene / bloom / LUT)',
    '  0 / 4             postfx all off / all on',
    '',
    'Music: put mp3/wav/flac/ogg/m4a into vsbuild/Assets/Music/',
    '       and play it from the Audio panel in the console (~).'
)
$readme = $readmeLines -join "`n"
Set-Content -Path "$pkgRoot\README.txt" -Value $readme -Encoding UTF8

Write-Host '[6/6] Compressing...'
$zip = Join-Path $root 'AfterglowRenderDX12_Release.zip'
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $pkgRoot -DestinationPath $zip -CompressionLevel Optimal
$size = (Get-Item $zip).Length / 1MB
Write-Host ("Done: " + $zip + "  ({0:N1} MB)" -f $size)
