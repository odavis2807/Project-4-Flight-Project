@echo off
SET /A "index = 1"
SET /A "count = 300"

:while
if %index% leq %count% (
     START /MIN Client.exe 25.21.209.204 "katl-kefd-B737-700.txt"
     SET /A index = %index% + 1
     @echo %index%
     goto :while
)