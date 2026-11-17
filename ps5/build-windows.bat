@echo off
setlocal
rem Longshot for PS5: the build on a Windows PC, through Ubuntu 24.04 under WSL 2.
rem
rem   Drag your game's .nup file onto this file, or run:  build-windows.bat C:\path\to\your-game.nup [console address]
rem
rem It runs build-title.sh (the Linux build: keep it in the same folder as this file) inside Ubuntu, in Ubuntu's own
rem files (~/PS5_Homebrews), and puts the finished PPSA99025 folder beside this file. Given the console's address, it
rem also sends the title and your game file to the console over FTP. Run it again after an interruption: what is done
rem is not done again. Nothing made from your game leaves your PC, except to the console you name.
set "DISTRO=Ubuntu-24.04"

echo.
echo  Longshot for PS5 - build
echo  ========================
echo.

if "%~1"=="" (
    echo Drag your game's .nup file onto build-windows.bat, or run:
    echo     build-windows.bat C:\path\to\your-game.nup
    goto :fail
)
if not exist "%~f1" (
    echo No such file: %~f1
    goto :fail
)
if not exist "%~dp0build-title.sh" (
    echo build-title.sh must be in the same folder as this file: %~dp0
    goto :fail
)

wsl.exe -d %DISTRO% -- true >nul 2>&1
if errorlevel 1 (
    echo Ubuntu 24.04 under WSL is not set up yet. Open PowerShell as administrator and run:
    echo.
    echo     wsl --install -d %DISTRO%
    echo.
    echo Restart Windows if it asks, open "Ubuntu 24.04" once from the Start menu to choose a user name and a
    echo password, then run this file again.
    goto :fail
)

rem (WSL gets half of the PC's memory and a small swap: with 8 GB the compiler ran out and Ubuntu was restarted, killing
rem the build. Without a .wslconfig of the user's own, one with 16 GB of swap, and WSL restarted so that it applies)
if not exist "%USERPROFILE%\.wslconfig" (
    > "%USERPROFILE%\.wslconfig" (
        echo [wsl2]
        echo swap=16GB
    )
    echo Made %USERPROFILE%\.wslconfig with 16 GB of swap for WSL, so that the build does not run out of memory.
    wsl.exe --shutdown
)

set "PS5_ADDRESS=%~2"
if "%PS5_ADDRESS%"=="" (
    echo To send the title to your PS5 when it is built, type the console's IP address ^(for example 192.168.1.50^),
    echo with its homebrew environment and FTP server running. To copy it yourself, just press Enter.
    set /p "PS5_ADDRESS=PS5 address: "
)

rem (the paths go to Ubuntu as its own paths: WSLENV's /p)
set "LONGSHOT_GAME=%~f1"
set "LONGSHOT_OUT=%~dp0."
set "LONGSHOT_SCRIPT=%~dp0build-title.sh"
set "LONGSHOT_PS5=%PS5_ADDRESS%"
set "WSLENV=LONGSHOT_GAME/p:LONGSHOT_OUT/p:LONGSHOT_SCRIPT/p:LONGSHOT_PS5"

echo.
echo == Ubuntu's packages (Ubuntu may ask for the password you chose for it)
wsl.exe -d %DISTRO% -u root -- bash -c "tr -d '\r' < \"$LONGSHOT_SCRIPT\" > /tmp/longshot-build-title.sh && bash /tmp/longshot-build-title.sh --install-packages"
if errorlevel 1 goto :fail

echo.
echo == The build: an hour or more the first time, minutes after that
wsl.exe -d %DISTRO% --cd ~ -- bash -c "tr -d '\r' < \"$LONGSHOT_SCRIPT\" > ~/longshot-build-title.sh && bash ~/longshot-build-title.sh"
if errorlevel 1 goto :fail

echo.
echo Done: %~dp0PPSA99025
if not "%PS5_ADDRESS%"=="" echo It is on your PS5 too: start Longshot from the home screen.
echo The first start prepares every shader ^(about 2.5 hours^) and closes by itself; the second goes to the game.
pause
exit /b 0

:fail
echo.
echo The build did not finish.
pause
exit /b 1
