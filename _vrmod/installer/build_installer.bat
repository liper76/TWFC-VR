@echo off
rem Builds the mod's setup: ..\dist\TWFC-VR-Setup.exe and TWFC-VR-Setup-Steam.exe (run ..\build.bat first for xinput1_3.dll).
rem Needs only the .NET Framework 4 compiler that ships with Windows.
setlocal
set HERE=%~dp0
set GAME=%HERE%..\..\
set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
set STAGE=%HERE%..\build\setup
if not exist "%STAGE%" mkdir "%STAGE%"
if not exist "%HERE%..\dist" mkdir "%HERE%..\dist"

rem the controller layout pictures (also kept in _vrmod\docs for the README)
"%CSC%" /nologo /codepage:65001 /target:exe /r:System.Drawing.dll /out:"%STAGE%\ControlsImage.exe" "%HERE%ControlsImage.cs" || exit /b 1
"%STAGE%\ControlsImage.exe" "%HERE%..\docs" "%HERE%..\docs\controllers_photo.png" || exit /b 1

copy /y "%HERE%..\build\xinput1_3.dll" "%STAGE%\xinput1_3.dll" >nul || exit /b 1
copy /y "%HERE%..\..\_vrmod_tools\openxr\Win32\bin\openxr_loader.dll" "%STAGE%\openxr_loader.dll" >nul || exit /b 1
copy /y "%GAME%BinariesVR\D3D9.dll" "%STAGE%\D3D9.dll" >nul || exit /b 1
copy /y "%GAME%BinariesVR\dgVoodoo.conf" "%STAGE%\dgVoodoo.conf" >nul || exit /b 1
copy /y "%GAME%Launch VR.bat" "%STAGE%\launch_en.bat" >nul || exit /b 1
copy /y "%GAME%Launch VR.bat" "%STAGE%\launch_ru.bat" >nul || exit /b 1
copy /y "%GAME%VR mod - instructions.txt" "%STAGE%\readme_en.txt" >nul || exit /b 1
copy /y /b "%GAME%VR-*.txt" "%STAGE%\readme_ru.txt" >nul || exit /b 1

set R=
for %%f in (xinput1_3.dll openxr_loader.dll D3D9.dll dgVoodoo.conf launch_en.bat launch_ru.bat readme_en.txt readme_ru.txt) do call set R=%%R%% /resource:"%STAGE%\%%f",%%f
set R=%R% /resource:"%HERE%..\docs\controls_en.png",controls_en.png /resource:"%HERE%..\docs\controls_ru.png",controls_ru.png
"%CSC%" /nologo /codepage:65001 /target:winexe /optimize+ /r:System.Windows.Forms.dll /r:System.Drawing.dll %R% /out:"%HERE%..\dist\TWFC-VR-Setup.exe" "%HERE%Installer.cs" || exit /b 1
rem the same setup for the Steam version (always writes BinariesVR\steam_appid.txt, no version warning)
"%CSC%" /nologo /codepage:65001 /target:winexe /optimize+ /define:STEAM /r:System.Windows.Forms.dll /r:System.Drawing.dll %R% /out:"%HERE%..\dist\TWFC-VR-Setup-Steam.exe" "%HERE%Installer.cs" || exit /b 1
echo SETUP OK: _vrmod\dist\TWFC-VR-Setup.exe, _vrmod\dist\TWFC-VR-Setup-Steam.exe
