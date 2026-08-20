cmake_minimum_required(VERSION 3.16)

# Wi-Fi credentials belong in sdkconfig.defaults.local, which is gitignored.
# Later files in SDKCONFIG_DEFAULTS win, so anything set there overrides the
# checked-in defaults without the secret ever entering version control.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
else()
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_petcam)
