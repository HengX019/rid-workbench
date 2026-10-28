@echo off
powershell -NoProfile -Command "$p=Join-Path ([Environment]::GetFolderPath('Startup')) 'RID Workbench.lnk'; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p }; Write-Host 'Startup shortcut removed. Close the client using 退出监听.'"
pause
