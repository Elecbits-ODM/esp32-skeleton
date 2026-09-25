cmake_minimum_required(VERSION 3.16)

# This example lives inside the eb_azure component repo, so its sibling
# dependencies are referenced relative to this file. When you copy this
# example out of the repo, update these paths to wherever you've laid out
# eb_azure, eb_storage, eb_network, and azure-iot-middleware-freertos as
# sibling checkouts.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../.."             # eb_azure (this component)
    "${CMAKE_CURRENT_LIST_DIR}/../../../eb_storage"  # sibling: eb_storage
    "${CMAKE_CURRENT_LIST_DIR}/../../../eb_network"  # sibling: eb_network
    # You must also vendor azure-iot-middleware-freertos and add its path here,
    # e.g.:
    # "${CMAKE_CURRENT_LIST_DIR}/../../../azure-iot-middleware-freertos"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(azure_telemetry_demo)
