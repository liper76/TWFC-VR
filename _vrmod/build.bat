@echo off
rem Builds xinput1_3.dll (x86) and deploys it to ..\BinariesVR
setlocal
set ROOT=%~dp0
set TOOLS=%ROOT%..\_vrmod_tools
set MH=%TOOLS%\minhook\minhook-1.3.4
set XR=%TOOLS%\openxr
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist "%ROOT%build" mkdir "%ROOT%build"
pushd "%ROOT%build"
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS ^
  /I"%MH%\include" /I"%XR%\include" ^
  "%ROOT%src\main.cpp" "%ROOT%src\xinput_proxy.cpp" "%ROOT%src\game.cpp" "%ROOT%src\vr.cpp" "%ROOT%src\debug.cpp" "%ROOT%src\focus.cpp" "%ROOT%src\camera.cpp" "%ROOT%src\registry.cpp" "%ROOT%src\d3d9modes.cpp" "%ROOT%src\d3dtrace.cpp" "%ROOT%src\ik.cpp" "%ROOT%src\weapons.cpp" "%ROOT%src\inputblock.cpp" "%ROOT%src\pull.cpp" "%ROOT%src\hints.cpp" "%ROOT%src\grenades.cpp" "%ROOT%src\melee.cpp" "%ROOT%src\gfx.cpp" ^
  "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde32.c" ^
  /Fe:xinput1_3.dll /link /MAP /DEF:"%ROOT%src\exports.def" "%XR%\Win32\lib\openxr_loader.lib" d3d11.lib dxgi.lib user32.lib gdi32.lib dxguid.lib
set ERR=%ERRORLEVEL%
popd
if not "%ERR%"=="0" exit /b %ERR%
copy /y "%ROOT%build\xinput1_3.dll" "%ROOT%..\BinariesVR\" >nul || (echo DEPLOY FAILED - game running? & exit /b 1)
copy /y "%XR%\Win32\bin\openxr_loader.dll" "%ROOT%..\BinariesVR\" >nul
echo BUILD OK



