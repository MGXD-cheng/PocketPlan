# Run an arbitrary command inside the ESP-IDF v6.1 environment.
# Usage example:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File "D:\Users\PC\Desktop\esp32-mgxd\tools\idfenv.ps1" -Command "idf.py --version"
param(
    [Parameter(Mandatory = $true)]
    [string]$Command,

    [string]$WorkDir = "D:\Users\PC\Desktop\esp32-mgxd\xz\src\xiaozhi-esp32-main"
)

$env:IDF_PATH = 'D:\esp\esp-idf-v6.1'
$env:IDF_TOOLS_PATH = 'C:\Users\PC\.espressif'
$env:PYTHONUTF8 = '1'
$env:PYTHONIOENCODING = 'utf-8'

. 'D:\esp\esp-idf-v6.1\export.ps1' | Out-Null

Set-Location $WorkDir
& ([scriptblock]::Create($Command))

