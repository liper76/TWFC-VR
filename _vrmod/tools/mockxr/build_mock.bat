@echo off
setlocal
set ROOT=%~dp0
set XR=%ROOT%..\..\..\_vrmod_tools\openxr
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
pushd "%ROOT%"
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /D_CRT_SECURE_NO_WARNINGS /I"%XR%\include" mockxr.cpp /Fe:mockxr.dll /link d3d11.lib /EXPORT:xrNegotiateLoaderRuntimeInterface=_xrNegotiateLoaderRuntimeInterface@8
set ERR=%ERRORLEVEL%
popd
if not "%ERR%"=="0" exit /b %ERR%
> "%ROOT%mockxr.json" echo {"file_format_version": "1.0.0", "runtime": {"name": "MockXR", "library_path": ".\\mockxr.dll"}}
echo MOCK BUILD OK

