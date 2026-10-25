juce_add_console_app (SerialPortTests
    PRODUCT_NAME "SerialPortTests")

target_sources (SerialPortTests
    PRIVATE
        Main.cpp
        Links.h)

target_compile_definitions (SerialPortTests
    PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0)

target_link_libraries (SerialPortTests
    PRIVATE
        juce_serialport
        juce::juce_core
        juce::juce_events
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# openpty
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries (SerialPortTests PRIVATE util)
endif ()
