@echo off
rem Builds "FML Cover Art.exe" (one file, no Python needed to run it) into dist\
rem Needs Python with Pillow and PyInstaller:  python -m pip install pillow pyinstaller
cd /d "%~dp0.."
python -m PyInstaller --noconfirm --clean --onefile --windowed --name "FML Cover Art" ^
  --add-data "%CD%\tools\fonts;fonts" --paths "%CD%\tools" ^
  --workpath build\pyinstaller --specpath build\pyinstaller ^
  tools\cover_art_app.py
