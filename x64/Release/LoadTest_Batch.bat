@echo off
SET /A "index = 1"
SET /A "count = 25"
:while
if %index% leq %count% (
     START /MIN Client.exe 25.21.209.204 "Telem_2023_3_12 16_26_4.txt"
     SET /A index = %index% + 1
     @echo %index%
     timeout /t 1 /nobreak > NUL
     goto :while
)
