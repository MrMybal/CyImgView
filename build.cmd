@echo off
rem Compile CyImgView (Release par défaut) : build.cmd [Release^|Debug]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
