@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
cd /d f:\project\VLR_WF\libVLR
cl /c /std:c++17 /EHsc /I"C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1\include" /I"C:\ProgramData\NVIDIA Corporation\OptiX SDK 8.0.0\include" /Iinclude /Ishared /Iutils context_test.cpp
