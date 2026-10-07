add_executable(es1asyncclient AsyncJsonRpcClient.cpp)

set_target_properties(es1asyncclient PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES
)

find_package(Threads REQUIRED)
target_link_libraries(es1asyncclient PRIVATE Threads::Threads)

install(TARGETS es1asyncclient
    DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)