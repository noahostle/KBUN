cmake_minimum_required(VERSION 3.21)

project(KBUN VERSION 0.1.0 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(KBUN WIN32
    src/main.cpp
    src/app.cpp
    src/automation.cpp
    src/config.cpp
    src/hints.cpp
    src/overlay.cpp
    resources/KBUN.rc
)

target_include_directories(KBUN PRIVATE src)
target_compile_definitions(KBUN PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(KBUN PRIVATE /W4 /permissive- /EHsc /utf-8 /O2)
else()
    target_compile_options(KBUN PRIVATE -Wall -Wextra -Wpedantic -O3)
endif()

target_link_libraries(KBUN PRIVATE
    advapi32
    dwmapi
    gdi32
    ole32
    oleaut32
    shell32
    shlwapi
    uiautomationcore
    user32
)

include(CTest)
if(BUILD_TESTING)
    add_executable(kbun_hints_test
        tests/hints_test.cpp
        src/hints.cpp
    )
    target_include_directories(kbun_hints_test PRIVATE src)
    target_compile_definitions(kbun_hints_test PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    add_test(NAME hints COMMAND kbun_hints_test)
endif()

