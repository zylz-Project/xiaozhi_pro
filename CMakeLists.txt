# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

# Select the maintained project fork without modifying vendored components.
file(GLOB PROJECT_COMPONENT_PATHS LIST_DIRECTORIES true "${CMAKE_CURRENT_LIST_DIR}/components/*")
list(REMOVE_ITEM PROJECT_COMPONENT_PATHS "${CMAKE_CURRENT_LIST_DIR}/components/esp-wifi-connect")
set(COMPONENT_DIRS "$ENV{IDF_PATH}/components" "${CMAKE_CURRENT_LIST_DIR}/main"
    ${PROJECT_COMPONENT_PATHS} "${CMAKE_CURRENT_LIST_DIR}/project_components/esp-wifi-connect")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "2.4.1")
project(xiaozhi)
