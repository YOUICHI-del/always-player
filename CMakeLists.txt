cmake_minimum_required(VERSION 3.20)
project(Always VERSION 6.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /arch:SSE2")

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
set(taglib_DIR "C:/vcpkg/installed/x64-windows/share/taglib")
find_package(taglib CONFIG REQUIRED)

set(MPV_INCLUDE_DIR "C:/libs/mpv/include")
set(MPV_LIB_DIR "C:/libs/mpv")

# ★ ビルドタイムスタンプ（例: 20260606-1530）
string(TIMESTAMP BUILD_TIMESTAMP "%Y%m%d-%H%M")

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
    src/CdDrive.h
    src/CdDrive.cpp
    src/RingBuffer.h
    src/CDReader.h
    src/CDReader.cpp
    src/CdStreamWriter.h
    src/CdStreamWriter.cpp
    src/CdMetaFetcher.h        # ★ 新規追加
    src/CdMetaFetcher.cpp      # ★ 新規追加
    src/MusicBrainz.h          # ★ 新規追加
    src/MusicBrainz.cpp        # ★ 新規追加
    src/CoverArt.h             # ★ 新規追加
    src/CoverArt.cpp           # ★ 新規追加
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
    TagLib::tag
    ${MPV_LIB_DIR}/libmpv.dll.a
    winhttp.lib
    powrprof.lib
    winmm.lib
)

target_compile_definitions(Always PRIVATE
    UNICODE _UNICODE
    BUILD_TIMESTAMP="${BUILD_TIMESTAMP}"   # ★ ビルド日時マクロ
)
