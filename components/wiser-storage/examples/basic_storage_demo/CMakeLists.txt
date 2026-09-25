cmake_minimum_required(VERSION 3.16)

# Point the build at the eb_storage component two levels up (repo root),
# so this example can be built standalone without installing the
# component anywhere else.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(basic_storage_demo)
