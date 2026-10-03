# Always Player MSIX builder (no capture needed)
# Usage (PowerShell):
#   cd C:\Users\user\Documents\Always_App_v10.0
#   powershell -ExecutionPolicy Bypass -File .\make_msix.ps1

$src       = "C:\Users\user\Documents\Always_App_v10.0"   # app folder
$exe       = "Always.exe"                                 # main exe
$ver       = "10.0.4.0"                                   # must be higher than the last submitted (10.0.3.0)
$name      = "YOUICHISAIJO.AlwaysPlayer"
$publisher = "CN=36731CB0-37AA-4672-9172-CA0742DFD1B4"
$pubName   = "YOUICHI SAIJO"

$work = Join-Path $env:TEMP "AlwaysPlayer_msix"
$out  = Join-Path ([Environment]::GetFolderPath("Desktop")) ("AlwaysPlayer_" + $ver + "_x64.msix")

if (-not (Test-Path (Join-Path $src $exe))) {
    Write-Host "ERROR: $exe not found in $src" -ForegroundColor Red
    Get-ChildItem $src -Filter *.exe | ForEach-Object { Write-Host "  found: $($_.Name)" }
    exit 1
}

# 1. copy only the runtime files (development files are excluded)
$excludeDirs  = @(".git", ".vs", "out", "src", "engine", "gapless_test", "third_party")
$excludeFiles = @("*.wxs", "*.wixobj", "*.wixpdb", "*.msi", "*.msix", "*.bat", "*.ps1", "*.log",
                  "*.csv", "*.ini", "*.bak", "*.qrc", "CMakeLists.txt", "CMakePresets.json", ".gitignore")

if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item $work -ItemType Directory -Force | Out-Null

Get-ChildItem $src -Force | ForEach-Object {
    $item = $_
    if ($item.PSIsContainer) {
        if ($excludeDirs -notcontains $item.Name) {
            Copy-Item $item.FullName (Join-Path $work $item.Name) -Recurse -Force
        }
    } else {
        $skip = $false
        foreach ($pat in $excludeFiles) { if ($item.Name -like $pat) { $skip = $true } }
        if (-not $skip) { Copy-Item $item.FullName $work -Force }
    }
}
Get-ChildItem $work -Include *.ini,*.bak -Recurse | Remove-Item -Force

# 2. logos from the exe icon
$assets = Join-Path $work "Assets"
New-Item $assets -ItemType Directory -Force | Out-Null
Add-Type -AssemblyName System.Drawing
$icon = [System.Drawing.Icon]::ExtractAssociatedIcon((Join-Path $src $exe))
$srcBmp = $icon.ToBitmap()
function Save-Logo([int]$size, [string]$file) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.DrawImage($script:srcBmp, 0, 0, $size, $size)
    $g.Dispose()
    $bmp.Save((Join-Path $script:assets $file), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
Save-Logo 50  "StoreLogo.png"
Save-Logo 150 "Square150x150Logo.png"
Save-Logo 44  "Square44x44Logo.png"

# 3. manifest
$manifest = @"
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
         IgnorableNamespaces="uap rescap">
  <Identity Name="$name" Publisher="$publisher" Version="$ver" ProcessorArchitecture="x64" />
  <Properties>
    <DisplayName>Always Player</DisplayName>
    <PublisherDisplayName>$pubName</PublisherDisplayName>
    <Description>High-fidelity audio player</Description>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>
  <Resources>
    <Resource Language="en-us" />
  </Resources>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.17763.0" MaxVersionTested="10.0.22621.0" />
  </Dependencies>
  <Applications>
    <Application Id="AlwaysPlayer" Executable="$exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements DisplayName="Always Player" Description="High-fidelity audio player"
                          BackgroundColor="transparent"
                          Square150x150Logo="Assets\Square150x150Logo.png"
                          Square44x44Logo="Assets\Square44x44Logo.png" />
    </Application>
  </Applications>
  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>
</Package>
"@
[System.IO.File]::WriteAllText((Join-Path $work "AppxManifest.xml"), $manifest, (New-Object System.Text.UTF8Encoding $false))

# 4. pack
$makeappx = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Recurse -Filter makeappx.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like "*\x64\*" } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $makeappx) {
    Write-Host "ERROR: makeappx.exe not found. Install Windows SDK." -ForegroundColor Red
    exit 1
}
& $makeappx.FullName pack /d $work /p $out /o
if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "DONE: $out" -ForegroundColor Green
} else {
    Write-Host "makeappx failed." -ForegroundColor Red
}
