@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /EHsc /Fe:mftest.exe mftest.cpp MovieMF.cpp user32.lib gdi32.lib ole32.lib mfplay.lib mfplat.lib mf.lib mfuuid.lib
