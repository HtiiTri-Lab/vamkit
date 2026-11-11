@echo off
rem Opens the vamkit launch panel: the mod or vanilla VaM, VR or desktop. No game starts until you click Launch.
rem Installed beside VaM.exe; the panel itself is vamkit\launcher\LaunchPanel.py.
cd /d "%~dp0"
where pyw >nul 2>nul && (start "" pyw -3 "%~dp0vamkit\launcher\LaunchPanel.py" & exit /b 0)
where pythonw >nul 2>nul && (start "" pythonw "%~dp0vamkit\launcher\LaunchPanel.py" & exit /b 0)
echo Python 3.11 or later was not found on this PC. Install it from python.org, then run this file again.
pause
