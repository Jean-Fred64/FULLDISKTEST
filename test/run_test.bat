@ECHO OFF
SET IMG=test\template_360.img
IF NOT "%~1"=="" SET IMG=%~1
COPY /Y "%IMG%" test\work.img > NUL
"D:\JeanFred\Mes Documents\Divers\DOSBox-X\bin\mingw-build\mingw\dosbox-x.exe" -defaultconf -nopromptfolder -fastlaunch -c "cycles max" -c "mount d ." -c "imgmount a test\work.img -t floppy" -c "d:" -c "build\FULLDSKT.EXE A: /Y > test\result.txt" -c "exit"

