# Pure logic (no QtWidgets) — testable headless
add_library(orbitgui_logic STATIC
    FileType.cpp
    UrlName.cpp
    GridGeometry.cpp
    SpeedSampler.cpp
    DropTargets.cpp
    Settings.cpp
    Scheduler.cpp
    BrowserBridgeProtocol.cpp
    BrowserBridge.cpp
)
target_link_libraries(orbitgui_logic PUBLIC orbitcore Qt6::Core Qt6::Network)
target_include_directories(orbitgui_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Widgets + model
add_library(orbitgui STATIC
    DownloadTableModel.cpp
    CategoryFilterProxy.cpp
    ProgressGridWidget.cpp
    NewDownloadDialog.cpp
    CategoryTree.cpp
    ClipboardWatcher.cpp
    CredentialsDialog.cpp
    PreferencesDialog.cpp
    SchedulerDialog.cpp
    MainWindow.cpp
)
target_link_libraries(orbitgui PUBLIC orbitgui_logic orbitcore Qt6::Widgets Qt6::Core)
target_include_directories(orbitgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Thin executable
add_executable(orbit-gui main_gui.cpp)
target_link_libraries(orbit-gui PRIVATE orbitgui)
