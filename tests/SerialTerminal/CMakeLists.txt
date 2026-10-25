# Windows, macOS, Linux and iOS. Android is built from SerialTerminal.jucer, see ../README.md
juce_add_gui_app (SerialTerminal
    PRODUCT_NAME "SerialTerminal"
    COMPANY_NAME "om ohm productions"
    BUNDLE_ID "com.omohmproductions.serialterminal")

target_sources (SerialTerminal
    PRIVATE
        Source/Main.cpp
        Source/MainComponent.cpp
        Source/MainComponent.h)

target_compile_definitions (SerialTerminal
    PRIVATE
        JUCE_USE_CURL=0
        JUCE_WEB_BROWSER=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SerialTerminal,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SerialTerminal,JUCE_VERSION>")

target_link_libraries (SerialTerminal
    PRIVATE
        juce_serialport
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
