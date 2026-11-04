@echo off
rem Builds richtext_probe.exe against qrenderdoc's bundled Qt (x64), from a
rem "x64 Native Tools Command Prompt for VS". Run it from this directory.
setlocal
set QT=..\..\..\qrenderdoc\3rdparty\qt\x64
set QTINC=..\..\..\qrenderdoc\3rdparty\qt\include
if not exist out mkdir out
cl /nologo /EHsc /O2 /MD /DUNICODE /D_UNICODE /I%QTINC% /I%QTINC%\QtCore /I%QTINC%\QtGui /I%QTINC%\QtWidgets main.cpp /Fe:out\richtext_probe.exe /Fo:out\ /link /LIBPATH:%QT%\lib Qt5Widgets.lib Qt5Gui.lib Qt5Core.lib /SUBSYSTEM:CONSOLE || exit /b 1
copy /y %QT%\bin\Qt5Core.dll out\ >nul
copy /y %QT%\bin\Qt5Gui.dll out\ >nul
copy /y %QT%\bin\Qt5Widgets.dll out\ >nul
if not exist out\platforms mkdir out\platforms
copy /y %QT%\plugins\platforms\qwindows.dll out\platforms\ >nul
copy /y ..\..\..\qrenderdoc\Resources\link.png out\ >nul
echo Built out\richtext_probe.exe
