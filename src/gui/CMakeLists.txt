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

# Thin executable — bundle Orbit.app no macOS, executável comum nas demais plataformas
if(APPLE)
    set(ORBIT_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/Orbit.icns")
    set_source_files_properties(${ORBIT_APP_ICON} PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources")
    add_executable(orbit-gui MACOSX_BUNDLE main_gui.cpp ${ORBIT_APP_ICON})
    set_target_properties(orbit-gui PROPERTIES
        OUTPUT_NAME "Orbit"
        MACOSX_BUNDLE_BUNDLE_NAME "Orbit"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.rootless.orbit-downloader-tribute"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "Orbit.icns"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
else()
    add_executable(orbit-gui main_gui.cpp)
endif()
target_link_libraries(orbit-gui PRIVATE orbitgui)
