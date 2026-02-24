@echo off
REM Monitor serial output from ESP32 (Windows)

REM Check if venv exists
if not exist "venv\" (
    echo Error: Virtual environment not found.
    echo Please run setup.bat first to set up the development environment.
    exit /b 1
)

REM Activate virtual environment
call venv\Scripts\activate.bat

REM Check if pio is available
where pio >nul 2>nul
if %errorlevel% neq 0 (
    echo Error: PlatformIO not found in virtual environment.
    echo Please run setup.bat to install dependencies.
    exit /b 1
)

REM Monitor device
echo Starting serial monitor...
pio device monitor -e pico32
