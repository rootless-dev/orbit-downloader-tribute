add_executable(tst_smoke tst_smoke.cpp)
target_link_libraries(tst_smoke PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_smoke COMMAND tst_smoke)

add_executable(tst_segmentation tst_segmentation.cpp)
target_link_libraries(tst_segmentation PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_segmentation COMMAND tst_segmentation)

add_executable(tst_persistence tst_persistence.cpp)
target_link_libraries(tst_persistence PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_persistence COMMAND tst_persistence)

add_executable(tst_download tst_download.cpp TestServer.cpp)
target_link_libraries(tst_download PRIVATE orbitcore Qt6::Test Qt6::HttpServer)
add_test(NAME tst_download COMMAND tst_download)

add_executable(tst_transport tst_transport.cpp FakeTransport.cpp)
target_link_libraries(tst_transport PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_transport COMMAND tst_transport)

add_executable(tst_contentdisposition tst_contentdisposition.cpp)
target_link_libraries(tst_contentdisposition PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_contentdisposition COMMAND tst_contentdisposition)

add_executable(tst_gui tst_gui.cpp TestServer.cpp)
target_link_libraries(tst_gui PRIVATE orbitgui Qt6::Test Qt6::HttpServer)
add_test(NAME tst_gui COMMAND tst_gui)
set_tests_properties(tst_gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(tst_ftp tst_ftp.cpp TestFtpServer.cpp)
target_link_libraries(tst_ftp PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_ftp COMMAND tst_ftp)

add_executable(tst_settings tst_settings.cpp)
target_link_libraries(tst_settings PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_settings COMMAND tst_settings)

add_executable(tst_ratelimiter tst_ratelimiter.cpp)
target_link_libraries(tst_ratelimiter PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_ratelimiter COMMAND tst_ratelimiter)

add_executable(tst_scheduler tst_scheduler.cpp)
target_link_libraries(tst_scheduler PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_scheduler COMMAND tst_scheduler)

add_executable(tst_logger tst_logger.cpp)
target_link_libraries(tst_logger PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_logger COMMAND tst_logger)

add_executable(tst_browserbridge tst_browserbridge.cpp)
target_link_libraries(tst_browserbridge PRIVATE orbitgui_logic Qt6::Test Qt6::Network)
add_test(NAME tst_browserbridge COMMAND tst_browserbridge)

add_executable(tst_bootoptions tst_bootoptions.cpp)
target_link_libraries(tst_bootoptions PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_bootoptions COMMAND tst_bootoptions)

add_executable(tst_singleinstance tst_singleinstance.cpp)
target_link_libraries(tst_singleinstance PRIVATE orbitgui_logic Qt6::Test Qt6::Network)
add_test(NAME tst_singleinstance COMMAND tst_singleinstance)

add_executable(tst_autostart tst_autostart.cpp)
target_link_libraries(tst_autostart PRIVATE orbitgui_logic Qt6::Test)
add_test(NAME tst_autostart COMMAND tst_autostart)
