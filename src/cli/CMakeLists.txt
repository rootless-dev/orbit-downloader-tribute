add_executable(orbit-cli main.cpp)
target_link_libraries(orbit-cli PRIVATE orbitcore Qt6::Core Qt6::Network)
