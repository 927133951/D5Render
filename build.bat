@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 && cd /d g:\A_Develop\DD\build && cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --config Release
