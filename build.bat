@ECHO OFF
IF NOT EXIST build MKDIR build
wcl -0 -ms -os -wx -zq -fe=build\FULLDSKT.EXE src\fulldskt.c
IF ERRORLEVEL 1 (
    ECHO [ERREUR] Echec de la compilation Open Watcom.
    EXIT /B 1
)
DEL *.obj > NUL 2>&1
ECHO [OK] build\FULLDSKT.EXE genere avec succes.