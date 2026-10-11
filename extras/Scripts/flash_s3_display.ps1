# Build (and optionally upload) VanLiveConnect for the ESP32-S3 DevKitC-1 (N16R8) with the on-board
# 2.8" ILI9341 TFT + XPT2046 touch enabled.
#
# Usage (from the repository root):
#   .\extras\Scripts\flash_s3_display.ps1                 # compile only
#   .\extras\Scripts\flash_s3_display.ps1 -Port COM6      # compile and upload
#   .\extras\Scripts\flash_s3_display.ps1 -NoDisplay      # compile for the S3 without the TFT code
#   .\extras\Scripts\flash_s3_display.ps1 -Port COM6 -Display 35   # 3.5" 480x320 ILI9488 panel
#
# Requires: Arduino IDE 2 (for its bundled arduino-cli) or arduino-cli on PATH, ESP32 core 3.x, libraries
# VanBus, Async TCP (ESP32Async), ESP Async WebServer 3.9.x, TFT_eSPI, ArduinoJson.

param(
    [string]$Port = "",
    [string]$Display = "28",   # "28" = 2.8" 320x240 ILI9341, "35" = 3.5" 480x320 ILI9488
    [switch]$NoDisplay,
    [switch]$DebugSerial,
    [switch]$VerboseBuild
)

$ErrorActionPreference = "Stop"

$cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $cli) { $cli = "C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe" }
if (-not (Test-Path $cli)) { throw "arduino-cli not found. Install Arduino IDE 2 or put arduino-cli on PATH." }

$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$sketch = Join-Path $repo "VanLiveConnect"
$lib = Join-Path $env:USERPROFILE "Documents\Arduino\libraries"

$fqbn = "esp32:esp32:esp32s3"
$boardOptions = "CPUFreq=240,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,USBMode=hwcdc,CDCOnBoot=cdc,DebugLevel=none"

# TFT_eSPI takes its configuration from compiler defines. Keep in sync with the comment block in Config.h.
# Panel-specific defines
if ($Display -eq "35") {
    $panelFlags = @("-DILI9488_DRIVER=1", "-DTFT_WIDTH=320", "-DTFT_HEIGHT=480", "-DSPI_FREQUENCY=27000000", "-DSPI_READ_FREQUENCY=16000000")
} else {
    $panelFlags = @("-DILI9341_DRIVER=1", "-DTFT_WIDTH=240", "-DTFT_HEIGHT=320", "-DSPI_FREQUENCY=40000000", "-DSPI_READ_FREQUENCY=20000000")
}

$tftFlags = (@(
    "-DUSE_TFT_DISPLAY",
    "-DUSER_SETUP_LOADED=1",
    # On the ESP32-S3 with ESP32 core 3.x, TFT_eSPI 2.5.43 computes a zero register base for its default (FSPI)
    # port and crashes with StoreProhibited in init(). Using the HSPI port avoids that. The touch controller is
    # therefore put on FSPI in Display.ino.
    "-DUSE_HSPI_PORT",
    "-DTFT_SCLK=12", "-DTFT_MOSI=11", "-DTFT_MISO=14",
    "-DTFT_CS=10", "-DTFT_DC=9", "-DTFT_RST=8",
    "-DTFT_BL=7", "-DTFT_BACKLIGHT_ON=HIGH",
    "-DLOAD_GLCD", "-DLOAD_FONT2", "-DLOAD_FONT4", "-DLOAD_FONT6", "-DLOAD_FONT7", "-DLOAD_FONT8", "-DLOAD_GFXFF"
) + $panelFlags) -join " "

$cliArgs = @(
    "compile",
    "--fqbn", $fqbn,
    "--board-options", $boardOptions,
    "--library", (Join-Path $lib "ESP_Async_WebServer"),
    "--library", (Join-Path $lib "Async_TCP")
)

if (-not $NoDisplay) {
    $cliArgs += @(
        "--library", (Join-Path $lib "TFT_eSPI"),
        "--library", (Join-Path $lib "ArduinoJson"),
        "--build-property", "compiler.cpp.extra_flags=$tftFlags$(if ($DebugSerial) { ' -DDISPLAY_DEBUG_SERIAL' })"
    )
}

if ($Port) { $cliArgs += @("--upload", "-p", $Port) }
if ($VerboseBuild) { $cliArgs += "-v" }
$cliArgs += $sketch

Write-Host "arduino-cli $($cliArgs -join ' ')" -ForegroundColor DarkGray
& $cli @cliArgs
exit $LASTEXITCODE
