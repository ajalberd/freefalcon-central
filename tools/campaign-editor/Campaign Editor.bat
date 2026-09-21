@echo off
rem Launch the FreeFalcon campaign editor and open it in the default browser.
rem Pass a different game directory as the first argument if yours is not
rem C:\FreeFalcon6, e.g.  "Campaign Editor.bat" D:\Games\FreeFalcon6
setlocal
set GAMEDIR=%~1
if "%GAMEDIR%"=="" set GAMEDIR=C:\FreeFalcon6

where py >nul 2>nul && (
  py -3 "%~dp0server.py" --gamedir "%GAMEDIR%"
) || (
  python "%~dp0server.py" --gamedir "%GAMEDIR%"
)
if errorlevel 1 pause
endlocal
