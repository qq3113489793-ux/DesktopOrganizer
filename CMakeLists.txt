cmake_minimum_required(VERSION 3.24)
project(DesktopOrganizer LANGUAGES CXX RC)

add_executable(DesktopOrganizer WIN32
    src/main.cpp
    src/config.cpp
    src/diagnostic_log.cpp
    src/desktop_anchor.cpp
    src/live_desktop_capture.cpp
    src/resources.rc
)

target_compile_features(DesktopOrganizer PRIVATE cxx_std_20)
target_compile_definitions(DesktopOrganizer PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(DesktopOrganizer PRIVATE src)
target_link_libraries(DesktopOrganizer PRIVATE
    comctl32
    comdlg32
    dwmapi
    shell32
    shlwapi
    ole32
    oleaut32
    uuid
    advapi32
    uxtheme
    windowscodecs
    winmm
    d3d11
    dxgi
    windowsapp
    runtimeobject
)

if(MSVC)
    target_compile_options(DesktopOrganizer PRIVATE /W4 /permissive- /utf-8 /EHsc)
    # The application manifest (DPI awareness, Common-Controls v6, asInvoker)
    # is embedded through resources.rc; suppress the linker's auto-generated
    # manifest so the two copies do not collide.
    target_link_options(DesktopOrganizer PRIVATE /MANIFEST:NO)
endif()

install(TARGETS DesktopOrganizer RUNTIME DESTINATION .)

include(CTest)
if(BUILD_TESTING)
    add_executable(DesktopOrganizerTests tests/tests.cpp src/config.cpp src/desktop_anchor.cpp)
    target_compile_features(DesktopOrganizerTests PRIVATE cxx_std_20)
    target_compile_definitions(DesktopOrganizerTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_include_directories(DesktopOrganizerTests PRIVATE src)
    target_link_libraries(DesktopOrganizerTests PRIVATE shell32 ole32 uuid)
    if(MSVC)
        target_compile_options(DesktopOrganizerTests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
    add_test(NAME DesktopOrganizerTests COMMAND DesktopOrganizerTests)

    if(WIN32)
        add_executable(UiMotionProbe tests/ui_motion_probe.cpp)
        target_compile_features(UiMotionProbe PRIVATE cxx_std_20)
        target_compile_definitions(UiMotionProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(UiMotionProbe PRIVATE user32 gdi32)
        if(MSVC)
            target_compile_options(UiMotionProbe PRIVATE /W4 /permissive- /utf-8 /EHsc)
        endif()

        add_executable(LiveWallpaperCaptureProbe tests/live_wallpaper_capture_probe.cpp)
        target_compile_features(LiveWallpaperCaptureProbe PRIVATE cxx_std_20)
        target_compile_definitions(LiveWallpaperCaptureProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_include_directories(LiveWallpaperCaptureProbe PRIVATE src)
        target_link_libraries(LiveWallpaperCaptureProbe PRIVATE
            d3d11 dxgi windowsapp runtimeobject user32
        )
        if(MSVC)
            target_compile_options(LiveWallpaperCaptureProbe PRIVATE /W4 /permissive- /utf-8 /EHsc)
        endif()
    endif()
endif()
