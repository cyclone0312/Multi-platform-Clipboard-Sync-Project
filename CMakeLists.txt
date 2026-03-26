cmake_minimum_required(VERSION 3.16)

project(MultiPlatformClipboardSync VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 5.15.2 REQUIRED COMPONENTS Core Gui Network Widgets)

add_executable(clipboard_sync
    src/main.cpp
    src/app/AppController.h
    src/app/AppController.cpp
    src/clipboard/ClipboardMonitor.h
    src/clipboard/ClipboardMonitor.cpp
    src/clipboard/ClipboardWriter.h
    src/clipboard/ClipboardWriter.cpp
    src/sync/SyncCoordinator.h
    src/sync/SyncCoordinator.cpp
    src/transport/TransportServer.h
    src/transport/TransportServer.cpp
    src/transport/TransportClient.h
    src/transport/TransportClient.cpp
    src/protocol/ProtocolHeader.h
    src/protocol/MessageCodec.h
    src/protocol/MessageCodec.cpp
    src/common/Config.h
    src/common/Config.cpp
    src/common/Logger.h
    src/common/Logger.cpp
    src/ui/SyncDebugWindow.h
    src/ui/SyncDebugWindow.cpp
)

target_include_directories(clipboard_sync PRIVATE src)

target_link_libraries(clipboard_sync
    PRIVATE
        Qt5::Core
        Qt5::Gui
        Qt5::Network
        Qt5::Widgets
)

if(WIN32)
    target_link_libraries(clipboard_sync PRIVATE ws2_32)
endif()
