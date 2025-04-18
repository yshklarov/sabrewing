@echo off

@set BUILD_DIR=build
@set BIN_NAME=sabrewing.exe

@set SRC_DIR=..\src
@set EXT_DIR=..\ext
@set RES_DIR=..\res
@set IMGUI_DIR=%EXT_DIR%\imgui
@set IMPLOT_DIR=%EXT_DIR%\implot
@set SDL2_DIR=%EXT_DIR%\SDL2-2.32.4
@set FONTS_DIR=%RES_DIR%\fonts

@set INCLUDES=/I %FONTS_DIR% /I %IMGUI_DIR% /I %IMGUI_DIR%\backends /I %IMPLOT_DIR% /I "%DXSDK_DIR%/Include" /I %SDL2_DIR%\include
@set DEFINES=/D UNICODE /D _UNICODE /D _SILENCE_CXX17_C_HEADER_DEPRECATION_WARNING

@set SOURCES_IMGUI=%IMGUI_DIR%\imgui*.cpp %IMGUI_DIR%\backends\imgui_impl_sdl2.cpp %IMGUI_DIR%\backends\imgui_impl_opengl3.cpp
@set SOURCES_IMPLOT=%IMPLOT_DIR%\*.cpp
@set SOURCES_PROJ=%SRC_DIR%\gui_win32.cpp

@set LIBS=/LIBPATH:%SDL2_DIR%\lib\x64 SDL2.lib SDL2main.lib opengl32.lib shell32.lib

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

:: Build resources
rc /nologo /fo..\%BUILD_DIR%\resources.res ../res/resources.rc

:: Link
cl /nologo *.obj resources.res /Zi /link %LIBS% /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /OUT:%BIN_NAME% || goto :error

:: Copy DLLs to build directory
echo Copying DLLs...
copy %SDL2_DIR%\lib\x64\SDL2.dll . /b

:: Normal exit
popd
exit /b 0

:: Errored exit
:error
popd
exit /b %errorlevel%
