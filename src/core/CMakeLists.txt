add_library(orbitcore STATIC
  DownloadTypes.h
  AbstractTask.h
  Transport.h
  Segmentation.cpp
  ContentDisposition.cpp
  Persistence.cpp
  FtpReply.cpp
  FtpControlChannel.cpp
  FtpProbe.cpp
  FtpSegmentWorker.cpp
  FtpTransport.cpp
  HttpTransport.cpp
  HttpProbe.cpp
  SegmentWorker.cpp
  DownloadTask.cpp
  DownloadManager.cpp
  EngineConfigJson.cpp
  RateLimiter.cpp
  Logger.cpp
  torrent/Bencode.cpp
  torrent/TorrentMetainfo.cpp
  torrent/TrackerPeers.cpp
  torrent/Bitfield.cpp
  torrent/PieceStore.cpp
  torrent/PiecePicker.cpp
  torrent/ITrackerClient.h
  torrent/HttpTrackerClient.cpp
  torrent/UdpTrackerClient.h
  torrent/UdpTrackerClient.cpp
  torrent/PeerWire.cpp
  torrent/PeerConnection.cpp
  torrent/TorrentTask.cpp
  torrent/AnnounceController.h
  torrent/AnnounceController.cpp
)
set_target_properties(orbitcore PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(orbitcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbitcore PUBLIC Qt6::Core Qt6::Network)
