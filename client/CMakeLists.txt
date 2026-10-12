add_executable(es1asyncclient AsyncJsonRpcClient.cpp)
add_executable(AsyncJsonRpcLoadClient AsyncJsonRpcLoadClient.cpp)

set_target_properties(es1asyncclient PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES
)

set_target_properties(AsyncJsonRpcLoadClient PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES
)

find_package(Threads REQUIRED)
target_link_libraries(es1asyncclient PRIVATE Threads::Threads)
target_link_libraries(AsyncJsonRpcLoadClient PRIVATE Threads::Threads)

install(TARGETS es1asyncclient
    DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)

install(TARGETS AsyncJsonRpcLoadClient
    DESTINATION ${CMAKE_INSTALL_PREFIX}/bin
)
