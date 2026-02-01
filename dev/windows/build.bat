@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
