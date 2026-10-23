param(
    [string]$WorkDir = "D:\Users\PC\Desktop\esp32-mgxd\pocketplan",
    [string]$Command = "idf.py build",
    [string]$Log = ""
)

# Windows sets MSYSTEM globally on this machine (Git for Windows). ESP-IDF 6.x
# refuses to run in an MSys/Mingw environment, so drop it before the export
# script turns the Windows environment block into an IDF one.
$env:MSYSTEM = $null
$env:IDF_PATH = 'D:\esp\esp-idf-v6.1'
$env:IDF_TOOLS_PATH = 'C:\Users\PC\.espressif'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

$idf_exports = python "D:\esp\esp-idf-v6.1\tools\activate.py" --export
if (-not $idf_exports) {
    Write-Output "activate.py returned nothing"
    exit 1
}
. $idf_exports

Set-Location $WorkDir
if ($Log -ne "") {
    & ([scriptblock]::Create($Command)) *> $Log
    Write-Output ("exit=" + $LASTEXITCODE)
} else {
    & ([scriptblock]::Create($Command))
    Write-Output ("exit=" + $LASTEXITCODE)
}
