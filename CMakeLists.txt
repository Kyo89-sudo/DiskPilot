cmake_minimum_required(VERSION 3.21)
project(DiskPilot VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # runtime statico: un solo .exe, senza dipendere dai VC++ Redistributable
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(DiskPilot WIN32
    src/main.cpp
    src/Wmi.cpp
    src/DiskModel.cpp
    src/Ops.cpp
    src/Dialogs.cpp
    res/app.rc
)

target_compile_definitions(DiskPilot PRIVATE
    UNICODE _UNICODE NOMINMAX
    _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

target_link_libraries(DiskPilot PRIVATE
    ole32 oleaut32 wbemuuid comctl32 gdiplus uxtheme dwmapi shell32 dbghelp gdi32 user32 uuid
)

if(MSVC)
    target_compile_options(DiskPilot PRIVATE /W4 /utf-8 /MP /permissive- /wd4100)
    # il manifest (amministratore, DPI, Common Controls 6) è già dentro app.rc
    target_link_options(DiskPilot PRIVATE /MANIFEST:NO)
    target_link_options(DiskPilot PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
else()
    target_link_options(DiskPilot PRIVATE -municode -mwindows -static)
endif()
