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

add_executable(tst_gui tst_gui.cpp TestServer.cpp TestMetadataPeer.cpp)
target_link_libraries(tst_gui PRIVATE orbitgui Qt6::Test Qt6::HttpServer Qt6::Network)
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

add_executable(tst_bencode tst_bencode.cpp)
target_link_libraries(tst_bencode PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_bencode COMMAND tst_bencode)

add_executable(tst_metainfo tst_metainfo.cpp)
target_link_libraries(tst_metainfo PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_metainfo COMMAND tst_metainfo)

add_executable(tst_bitfield tst_bitfield.cpp)
target_link_libraries(tst_bitfield PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_bitfield COMMAND tst_bitfield)

add_executable(tst_piecestore tst_piecestore.cpp)
target_link_libraries(tst_piecestore PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_piecestore COMMAND tst_piecestore)

add_executable(tst_piecepicker tst_piecepicker.cpp)
target_link_libraries(tst_piecepicker PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_piecepicker COMMAND tst_piecepicker)

add_executable(tst_tracker tst_tracker.cpp)
target_link_libraries(tst_tracker PRIVATE orbitcore Qt6::Test Qt6::Network Qt6::HttpServer)
add_test(NAME tst_tracker COMMAND tst_tracker)

add_executable(tst_trackerpeers tst_trackerpeers.cpp)
target_link_libraries(tst_trackerpeers PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_trackerpeers COMMAND tst_trackerpeers)

add_executable(tst_peerwire tst_peerwire.cpp)
target_link_libraries(tst_peerwire PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_peerwire COMMAND tst_peerwire)

add_executable(tst_seeder tst_seeder.cpp TestSeeder.cpp)
target_link_libraries(tst_seeder PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_seeder COMMAND tst_seeder)

add_executable(tst_torrent tst_torrent.cpp TestSeeder.cpp TestUdpTracker.cpp TestMetadataPeer.cpp)
target_link_libraries(tst_torrent PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_torrent COMMAND tst_torrent)

add_executable(tst_udptracker tst_udptracker.cpp TestUdpTracker.cpp)
target_link_libraries(tst_udptracker PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_udptracker COMMAND tst_udptracker)

add_executable(tst_announcecontroller tst_announcecontroller.cpp)
target_link_libraries(tst_announcecontroller PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_announcecontroller COMMAND tst_announcecontroller)

add_executable(tst_magneturi tst_magneturi.cpp)
target_link_libraries(tst_magneturi PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_magneturi COMMAND tst_magneturi)

add_executable(tst_nodeid tst_nodeid.cpp)
target_link_libraries(tst_nodeid PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_nodeid COMMAND tst_nodeid)

add_executable(tst_routingtable tst_routingtable.cpp)
target_link_libraries(tst_routingtable PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_routingtable COMMAND tst_routingtable)

add_executable(tst_krpc tst_krpc.cpp)
target_link_libraries(tst_krpc PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_krpc COMMAND tst_krpc)

add_executable(tst_dht tst_dht.cpp)
target_link_libraries(tst_dht PRIVATE orbitcore Qt6::Test)
add_test(NAME tst_dht COMMAND tst_dht)

add_executable(tst_utmetadata tst_utmetadata.cpp TestMetadataPeer.cpp)
target_link_libraries(tst_utmetadata PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_utmetadata COMMAND tst_utmetadata)

add_executable(tst_metadatafetch tst_metadatafetch.cpp TestMetadataPeer.cpp)
target_link_libraries(tst_metadatafetch PRIVATE orbitcore Qt6::Test Qt6::Network)
add_test(NAME tst_metadatafetch COMMAND tst_metadatafetch)
