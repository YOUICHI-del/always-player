cmake_minimum_required(VERSION 3.20)
# ★ Cを追加：engine/wavpack/配下のlibwavpack(デコード専用サブセット)は
#   C言語ソース(.c)のため、CXXのみではCコンパイラが構成されず、
#   CMakeがコンパイルルールを生成できない（エラーも出さず黙って
#   ビルド対象から外れ、リンク時に未解決の外部シンボルとして発覚する）。
project(Always VERSION 10.0.0 LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network)
set(taglib_DIR "C:/vcpkg/installed/x64-windows/share/taglib" CACHE PATH "" FORCE)
find_package(taglib CONFIG REQUIRED)
set(MPV_INCLUDE_DIR "C:/libs/mpv/include")
set(MPV_LIB_DIR "C:/libs/mpv")
# Build timestamp
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
    src/WasapiLevelMeter.cpp
    src/WasapiLevelMeter.h
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
    src/CdMetaFetcher.h
    src/CdMetaFetcher.cpp
    src/GnuDb.h
    src/GnuDb.cpp
    src/MusicBrainz.h
    src/MusicBrainz.cpp
    src/CoverArt.h
    src/CoverArt.cpp
    src/RemoteServer.h
    src/RemoteServer.cpp
    engine/IAudioOutputBackend.h
    engine/WasapiExclusiveOutput.h
    engine/WasapiExclusiveOutput.cpp
    engine/RingBuffer.h
    engine/IPcmDecoder.h
    engine/FlacPcmDecoder.h
    engine/FlacPcmDecoder.cpp
    engine/WavPcmDecoder.h
    engine/WavPcmDecoder.cpp
    engine/AiffPcmDecoder.h
    engine/AiffPcmDecoder.cpp
    engine/WavPackPcmDecoder.h
    engine/WavPackPcmDecoder.cpp
    engine/MfPcmDecoder.h
    engine/MfPcmDecoder.cpp
    engine/OggVorbisPcmDecoder.h
    engine/OggVorbisPcmDecoder.cpp
    engine/stb_vorbis.c
    engine/OpusPcmDecoder.h
    engine/OpusPcmDecoder.cpp
    engine/PcmDualEngine.h
    engine/PcmDualEngine.cpp
    engine/SincResampler.h
    engine/SincResampler.cpp
    engine/AudioProcessThread.h
    engine/AudioProcessThread.cpp
    engine/HarmonicUpsampleDsp.h
    engine/HarmonicUpsampleDsp.cpp
    engine/DspChain.h
    engine/DspChain.cpp
    engine/AudioDeviceWatcher.h
    engine/AudioDeviceWatcher.cpp
    engine/wavpack/common_utils.c
    engine/wavpack/decorr_utils.c
    engine/wavpack/entropy_utils.c
    engine/wavpack/open_utils.c
    engine/wavpack/open_filename.c
    engine/wavpack/open_legacy.c
    engine/wavpack/open_raw.c
    engine/wavpack/read_words.c
    engine/wavpack/tags.c
    engine/wavpack/tag_utils.c
    engine/wavpack/unpack.c
    engine/wavpack/unpack_floats.c
    engine/wavpack/unpack_seek.c
    engine/wavpack/unpack_utils.c
    engine/wavpack/unpack_dsd.c
    engine/wavpack/wavpack.h
    engine/wavpack/wavpack_local.h
    engine/wavpack/wavpack_version.h
)
target_include_directories(Always PRIVATE
    ${MPV_INCLUDE_DIR}
    src
    engine
    engine/wavpack
    C:/vcpkg/installed/x64-windows/include
    C:/vcpkg/installed/x64-windows-static-md/include/opus
    C:/vcpkg/installed/x64-windows-static-md/include
)
find_package(Qt6 REQUIRED COMPONENTS Concurrent)
target_link_libraries(Always PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Network
    TagLib::tag
    ${MPV_LIB_DIR}/libmpv.dll.a
    winhttp.lib
    powrprof.lib
    winmm.lib
    mmdevapi.lib
    avrt.lib
    # v10: Opus（vcpkgの静的ライブラリ x64-windows-static-md。DLL不要）
    C:/vcpkg/installed/x64-windows-static-md/lib/opusfile.lib
    C:/vcpkg/installed/x64-windows-static-md/lib/opus.lib
    C:/vcpkg/installed/x64-windows-static-md/lib/ogg.lib
    mfplat.lib
    mfreadwrite.lib
    mfuuid.lib
    ole32.lib
)
target_compile_definitions(Always PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    BUILD_TIMESTAMP="${BUILD_TIMESTAMP}"
)
# ★ engine/配下の一部ファイルはBOMなしUTF-8で日本語コメントを含む。
#   Always本体はQt/AUTOMOC絡みでこれまで問題が出ていなかったが、
#   念のため明示的に/utf-8を指定し、コードページ依存の誤読を防ぐ。
target_compile_options(Always PRIVATE /utf-8)

# ★ ビルド後、最新のAlways.exeをプロジェクト直下へ自動コピーする。
#   out/build/... にはQtのDLL等が揃っておらずそこからは起動できないため、
#   DLL一式が揃っている直下のAlways.exeを常に最新に保つ（古いexeでテストしてしまう
#   取り違えの防止）。※Always.exeを起動したままビルドするとコピーに失敗するので、
#   ビルド前にアプリを閉じること。
add_custom_command(TARGET Always POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Always> ${CMAKE_SOURCE_DIR}/Always.exe
    COMMENT "Copying Always.exe to project root"
)

# --- AlphaStyleInterpolator standalone test (unrelated to production build) ---
add_executable(test_alphastyle
    engine/test_alphastyle.cpp
)
target_include_directories(test_alphastyle PRIVATE
    engine
)

add_executable(alphastyle_offline
    engine/alphastyle_offline.cpp
)
target_include_directories(alphastyle_offline PRIVATE
    engine
)

# --- TEST_Player: v9自作エンジン(dr_flac + WASAPI排他)の独立動作確認用コンソールアプリ ---
# Always本体（Qt/mpv）には一切依存しない。フォルダを開く→再生→一時停止→
# 停止→スキップ→バックの制御フローと、エンジン単体の音質を確認するためのもの。
add_executable(TEST_Player
    engine/EngineTestPlayer.cpp
    engine/IAudioOutputBackend.h
    engine/WasapiExclusiveOutput.h
    engine/WasapiExclusiveOutput.cpp
    engine/RingBuffer.h
    engine/FlacDualEngine.h
    engine/FlacDualEngine.cpp
    engine/AudioProcessThread.h
    engine/AudioProcessThread.cpp
)
target_include_directories(TEST_Player PRIVATE
    engine
)
target_link_libraries(TEST_Player PRIVATE
    winmm.lib
    mmdevapi.lib
    avrt.lib
    ole32.lib
)
target_compile_definitions(TEST_Player PRIVATE
    UNICODE _UNICODE
    NOMINMAX
)
# ★ Qtに依存しないターゲットのため、BOMなしUTF-8ファイル中の日本語コメントが
#   コードページ932(Shift-JIS)として誤読され、C2001/C2059/C4819等の
#   大量の構文エラーを引き起こしていた。/utf-8を明示してこれを解消する。
target_compile_options(TEST_Player PRIVATE /utf-8)
