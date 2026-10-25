# test applications for juce_serialport, see README.md
cmake_minimum_required (VERSION 3.22)

project (JuceSerialPortTests VERSION 1.0.0)

set (CMAKE_CXX_STANDARD 17)
set (CMAKE_CXX_STANDARD_REQUIRED ON)

set (JUCE_SOURCE_DIR "" CACHE PATH "A JUCE checkout. If empty, ../../JUCE is used if it exists (as in a project which has JUCE and this module side by side), otherwise JUCE 9.0.2 is downloaded")
if (NOT JUCE_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../JUCE/CMakeLists.txt")
    get_filename_component (JUCE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../JUCE" ABSOLUTE)
endif ()

if (JUCE_SOURCE_DIR)
    add_subdirectory ("${JUCE_SOURCE_DIR}" JUCE)
else ()
    include (FetchContent)
    FetchContent_Declare (JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.2
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable (JUCE)
endif ()

# the module is the parent of this folder, and must be in a folder called juce_serialport
get_filename_component (JUCE_SERIALPORT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
juce_add_module ("${JUCE_SERIALPORT_DIR}")

# the console test runner needs a desktop OS
if (NOT CMAKE_SYSTEM_NAME MATCHES "iOS|Android")
    add_subdirectory (SerialPortTests)
endif ()
add_subdirectory (SerialTerminal)
