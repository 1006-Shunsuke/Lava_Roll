@echo off
rem ---------------------------------------------------------------------------
rem make_dist.bat : Lava Roll の配布用フォルダと zip を作る
rem   1. Visual Studio で「Release | x64」をビルドしてから、このファイルをダブルクリックする
rem   2. dist\LavaRoll\ に exe・シェーダー（.cso）・dll・res をまとめ、dist\LavaRoll.zip を作る
rem ---------------------------------------------------------------------------
chcp 65001 >nul
setlocal
cd /d "%~dp0"

set BUILD=project\x64\Release
set OUT=dist\LavaRoll

rem Release でビルドされた exe があるか確かめる
if not exist "%BUILD%\Last.exe" (
    echo [ERROR] %BUILD%\Last.exe がありません。Visual Studio で「Release ^| x64」をビルドしてください。
    pause
    exit /b 1
)

rem 前回の dist を消して作り直す
if exist dist rmdir /s /q dist
mkdir "%OUT%"

rem exe（名前を LavaRoll.exe にする）・シェーダー・dll
copy /y "%BUILD%\Last.exe" "%OUT%\LavaRoll.exe" >nul
copy /y "%BUILD%\*.cso"    "%OUT%\" >nul
copy /y "%BUILD%\*.dll"    "%OUT%\" >nul

rem res フォルダ（バックアップの .bak は入れない）
robocopy res "%OUT%\res" /e /xf *.bak /njh /njs /ndl /nfl /nc /ns >nul

rem 遊び方の説明
copy /y "README.md" "%OUT%\README.md" >nul

rem zip にまとめる
powershell -NoProfile -Command "Compress-Archive -Path 'dist\LavaRoll' -DestinationPath 'dist\LavaRoll.zip' -Force"

echo.
echo 完了しました: dist\LavaRoll\ と dist\LavaRoll.zip
echo GitHub の Releases に dist\LavaRoll.zip を添付してください。
pause
