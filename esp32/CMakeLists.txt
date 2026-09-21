cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "src")
# Take the version from the file the keyboard firmware uses, rather than
# letting ESP-IDF fall back to `git describe`. That fallback reports the
# state of whatever working copy the build happened to run in -- which is
# how the bridge came to advertise itself as "d229c96-dirty" -- and says
# nothing useful about which firmware is installed.
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../version.txt" PROJECT_VER LIMIT_COUNT 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mpk_mini_ble_midi)
