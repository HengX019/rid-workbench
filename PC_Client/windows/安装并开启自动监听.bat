@echo off
setlocal
set "DEST=%LOCALAPPDATA%\RIDWorkbench"
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
  echo Windows .NET Framework C# compiler not found.
  pause
  exit /b 1
)
if not exist "%DEST%" mkdir "%DEST%"
"%CSC%" /nologo /codepage:65001 /target:winexe /optimize+ /out:"%DEST%\RIDClient.exe" /reference:System.Windows.Forms.dll /reference:System.Drawing.dll /reference:System.Management.dll "%~dp0RIDClient.cs"
if errorlevel 1 (
  echo Build failed. No startup shortcut was created.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path ([Environment]::GetFolderPath('Startup')) 'RID Workbench.lnk')); $s.TargetPath=Join-Path $env:LOCALAPPDATA 'RIDWorkbench\RIDClient.exe'; $s.Arguments='--background'; $s.WorkingDirectory=Join-Path $env:LOCALAPPDATA 'RIDWorkbench'; $s.Save()"
if errorlevel 1 (
  echo Could not create startup shortcut.
  pause
  exit /b 1
)
start "" "%DEST%\RIDClient.exe"
echo Installed. A startup shortcut was added for the current user.
pause
