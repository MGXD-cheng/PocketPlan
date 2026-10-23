param(
  [string]$Port = "COM5",
  [int]$Seconds = 10,
  [switch]$NoReset
)
$ErrorActionPreference = "Continue"
$p = New-Object System.IO.Ports.SerialPort($Port,115200,"None",8,"One")
$p.ReadTimeout = 300
$p.DtrEnable = $false
$p.RtsEnable = $false
$p.Open()
Start-Sleep -Milliseconds 250
if (-not $NoReset) {
  $p.RtsEnable = $true; Start-Sleep -Milliseconds 350; $p.RtsEnable = $false
}
$sb = New-Object System.Text.StringBuilder
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
  try { $s = $p.ReadExisting(); if ($s) { [void]$sb.Append($s) } } catch {}
  Start-Sleep -Milliseconds 60
}
$p.Close()
[Console]::Out.Write($sb.ToString())
