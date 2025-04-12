@echo off

@set BUILD_DIR=build
@set BIN_NAME=aworkshop.exe

@set SRC_DIR=..\src
@set IMGUI_DIR=..\ext\imgui
@set IMPLOT_DIR=..\ext\implot

@set INCLUDES=/I %IMGUI_DIR% /I %IMGUI_DIR%\backends /I %IMPLOT_DIR% /I "%DXSDK_DIR%/Include"
@set DEFINES=/D UNICODE /D _UNICODE /D _SILENCE_CXX17_C_HEADER_DEPRECATION_WARNING

@set SOURCES_IMGUI=%IMGUI_DIR%\imgui*.cpp %IMGUI_DIR%\backends\imgui_impl_dx9.cpp %IMGUI_DIR%\backends\imgui_impl_win32.cpp
@set SOURCES_IMPLOT=%IMPLOT_DIR%\*.cpp
@set SOURCES_PROJ=%SRC_DIR%\main_win32.cpp
::@set SOURCES=%SOURCES_PROJ% %SOURCES_IMGUI% %SOURCES_IMPLOT%

@set LIBS=/LIBPATH:"%DXSDK_DIR%/Lib/x86" d3d9.lib

:: Disable optimizations
:: For now, we do this for our targets.c so as to not "cheat".
@set OPTIMIZER_FLAGS=/Od
:: Optimize for speed
::@set OPTIMIZER_FLAGS=/O2

if not exist %BUILD_DIR% mkdir %BUILD_DIR%
pushd %BUILD_DIR%

:: Compile external libraries (slow due to /O2).
cl /nologo /c /W4 /Zi /FC /utf-8 /std:c++17 /O2 /Gm %INCLUDES% %DEFINES% %SOURCES_IMGUI% %SOURCES_IMPLOT% || goto :error

:: Compile our own code.
cl /nologo /c /W4 /Zi /FC /utf-8 /std:c++17 %OPTIMIZER_FLAGS% /MP %INCLUDES% %DEFINES% %SOURCES_PROJ% || goto :error

:: Link
cl /nologo *.obj /Zi /link %LIBS% /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /OUT:%BIN_NAME% || goto :error


:: Normal exit
popd
exit /b 0

:: Errored exit
:error
popd
exit /b %errorlevel%
