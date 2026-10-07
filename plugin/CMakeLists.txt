set(PLUGIN_NAME ES1BenchmarkAsync)
set(MODULE_NAME ${NAMESPACE}${PLUGIN_NAME})

set(PLUGIN_ES1BENCHMARKASYNC_AUTOSTART "true" CACHE STRING "Automatically start ES1BenchmarkAsync")
set(PLUGIN_ES1BENCHMARKASYNC_MODE "Local" CACHE STRING "Plugin execution mode")

find_package(${NAMESPACE}Plugins REQUIRED)
find_package(${NAMESPACE}Definitions REQUIRED)
find_package(CompileSettingsDebug CONFIG REQUIRED)

if(BUILD_REFERENCE)
    add_definitions(-DBUILD_REFERENCE=${BUILD_REFERENCE})
endif()

add_library(${MODULE_NAME} SHARED
    Module.cpp
    ES1BenchmarkAsync.cpp
)

set_target_properties(${MODULE_NAME} PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED YES
)
target_link_libraries(${MODULE_NAME}
    PRIVATE
        CompileSettingsDebug::CompileSettingsDebug
        ${NAMESPACE}Plugins::${NAMESPACE}Plugins
        ${NAMESPACE}Definitions::${NAMESPACE}Definitions
)
install(TARGETS ${MODULE_NAME}
    DESTINATION ${CMAKE_INSTALL_PREFIX}/lib/${STORAGE_DIRECTORY}/plugins
)

write_config(${PLUGIN_NAME})