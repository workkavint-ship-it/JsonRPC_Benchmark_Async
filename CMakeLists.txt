cmake_minimum_required(VERSION 3.3)

project(JsonRPCBenchmarkAsync)

find_package(WPEFramework)
include(CmakeHelperFunctions)

string(TOLOWER ${NAMESPACE} STORAGE_DIRECTORY)

option(PLUGIN_ES1BENCHMARKASYNC "Build the asynchronous ES1 benchmark plugin" ON)
option(PLUGIN_ES1BENCHMARKASYNC_CLIENT "Build the asynchronous ES1 benchmark client" ON)

if(PLUGIN_ES1BENCHMARKASYNC)
    add_subdirectory(plugin)
endif()

if(PLUGIN_ES1BENCHMARKASYNC_CLIENT)
    add_subdirectory(client)
endif()