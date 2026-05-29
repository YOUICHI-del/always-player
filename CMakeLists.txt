cmake_minimum_required(VERSION 3.20)
project(Always VERSION 5.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 汎用CPU向けビルド（Celeron等の低スペックPCにも対応）
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:SSE2")

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
set(taglib_DIR "C:/vcpkg/installed/x64-windows/share/taglib")
find_package(taglib CONFIG REQUIRED)

set(MPV_INCLUDE_DIR "C:/libs/mpv/include")
set(MPV_LIB_DIR "C:/libs/mpv")

add_executable(Always WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/Player.cpp
    src/Player.h
    src/TrayManager.cpp
    src/TrayManager.h
    src/VUMeter.cpp
    src/VUMeter.h
    resources/resources.qrc
    resources/app.rc
    src/TimerMonitor.h
    src/TimerMonitor.cpp
    src/UsbLatencyAnalyzer.h
    src/UsbLatencyAnalyzer.cpp
    src/ClockSculptor.h
    src/ClockSculptor.cpp
)

target_include_directories(Always PRIVATE
    ${MPV_INCLUDE_DIR}
    src
    C:/vcpkg/installed/x64-windows/include
)

find_package(Qt6 REQUIRED COMPONENTS Concurrent)

target_link_libraries(Always PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    "${MPV_LIB_DIR}/libmpv.dll.a"
    TagLib::tag
    ole32
    uuid
    winhttp
    powrprof
)

add_custom_command(TARGET Always POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MPV_LIB_DIR}/libmpv-2.dll"
        $<TARGET_FILE_DIR:Always>
)
