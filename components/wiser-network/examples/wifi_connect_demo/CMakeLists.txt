cmake_minimum_required(VERSION 3.16)

# This example lives inside the eb_network repo, two levels below the
# component root (examples/wifi_connect_demo/). eb_network is therefore
# picked up automatically via "../..". eb_storage is a separate sibling
# repo/component and is NOT part of this repo — adjust the path below to
# wherever you cloned/checked out eb_storage on your machine (e.g. if both
# repos are checked out side-by-side under the same parent directory, or
# under a shared "components/" folder).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${CMAKE_CURRENT_LIST_DIR}/../../../eb_storage"  # TODO: adjust to your eb_storage checkout path
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_connect_demo)
