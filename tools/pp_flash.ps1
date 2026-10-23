param(
    [string]$App = "D:\Users\PC\Desktop\esp32-mgxd\pocketplan\build\pocketplan.bin",
    [string]$Part = "D:\Users\PC\Desktop\esp32-mgxd\pocketplan\build\partition_table\partition-table.bin",
    [string]$Port = "COM5"
)
# Flash the partition table (its size changed: storage is now 1 MB) and the app.
# NVS at 0x9000 is untouched, so Wi-Fi credentials survive.
$python = "C:\Users\PC\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe"
& $python -m esptool --chip esp32s3 -p $Port -b 921600 --before default-reset --after hard-reset write-flash 0x8000 $Part 0x20000 $App
exit $LASTEXITCODE
