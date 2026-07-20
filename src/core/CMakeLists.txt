add_library(orbitcore STATIC
  DownloadTypes.h
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
)
set_target_properties(orbitcore PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(orbitcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbitcore PUBLIC Qt6::Core Qt6::Network)
