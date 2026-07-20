cmake_minimum_required(VERSION 3.21)
project(orbit_downloader_tribute VERSION 0.1.0 LANGUAGES CXX)

# A CI passa a versão da tag via -DORBIT_VERSION; localmente cai no default do project().
if(DEFINED ORBIT_VERSION AND NOT ORBIT_VERSION STREQUAL "")
  set(PROJECT_VERSION "${ORBIT_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_PREFIX_PATH)
  set(CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets HttpServer Test)

enable_testing()
add_subdirectory(src/core)
add_subdirectory(src/cli)
add_subdirectory(src/gui)
add_subdirectory(tests)
