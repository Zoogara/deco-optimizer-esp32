cmake_minimum_required(VERSION 3.16)
# Version shown on the device page and in the app descriptor: the short git hash (plus "-dirty" for local changes).
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
                OUTPUT_VARIABLE GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GIT_VERSION)
    set(PROJECT_VER "${GIT_VERSION}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(deco_optimizer)
