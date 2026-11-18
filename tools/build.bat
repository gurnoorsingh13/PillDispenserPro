@echo off
rem Build PillDispenser Pro (and optionally flash it).
rem   tools\build.bat          build only
rem   tools\build.bat COM11    build and flash over USB
rem Needs esp32 core 2.0.17 in %USERPROFILE%\arduino-esp32-2.0.17 (kept apart from the Arduino IDE's own core).
rem Do not build with core 3.x: Wi-Fi start crashes the board with the bundled libraries.
setlocal
set CLI="%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
set CFG=%USERPROFILE%\arduino-esp32-2.0.17\cli.yaml
set FQBN=esp32:esp32:esp32:PartitionScheme=min_spiffs,FlashFreq=80
cd /d "%~dp0.."

python tools\build_web.py || goto :err
%CLI% --config-file "%CFG%" compile --fqbn %FQBN% --libraries libraries --output-dir build PillDispenserPro || goto :err
for /f "tokens=3 delims= " %%v in ('findstr /c:"#define FW_VERSION" PillDispenserPro\app.h') do set VER=%%~v
copy /y build\PillDispenserPro.ino.bin release\PillDispenserPro-v%VER%-update.bin >nul
echo.
echo Built v%VER%: release\PillDispenserPro-v%VER%-update.bin (for Settings - Firmware in the web app)
if "%~1"=="" goto :eof

%CLI% --config-file "%CFG%" upload -p %1 --fqbn %FQBN% --input-dir build PillDispenserPro || goto :err
echo Flashed %1.
goto :eof

:err
echo.
echo BUILD FAILED
exit /b 1
