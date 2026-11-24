@echo off
REM Set ANDROID_NDK_HOME first, e.g.:
REM   setx ANDROID_NDK_HOME "C:\Program Files (x86)\Android\AndroidNDK\android-ndk-r2xx"
REM (VS 2022 installs the NDK with "Mobile development with C++"; check its folder)
REM Needs cmake + ninja on PATH (both ship with Visual Studio's CMake component).
if "%ANDROID_NDK_HOME%"=="" (echo Set ANDROID_NDK_HOME first & exit /b 1)
cmake --preset android-arm64 || exit /b 1
cmake --build --preset android-arm64 || exit /b 1
echo.
echo Output: out\build\arm64\libMalookys_Soundboard.so
