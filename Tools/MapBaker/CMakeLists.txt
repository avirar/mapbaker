if (WOWDECOMP_ARCH_64)
    set(MAPBAKER_OUTPUT_NAME MapBaker64)
else ()
    set(MAPBAKER_OUTPUT_NAME MapBaker32)
endif ()

file(GLOB MAPBAKER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
file(GLOB MAPBAKER_CLIENT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/WoW/Source/*.cpp")

if (WOWDECOMP_ARCH_64)
    file(GLOB MAPBAKER_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/WoW/Source/Win64/*.cpp")
else ()
    file(GLOB MAPBAKER_PLATFORM_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/WoW/Source/Win32/*.cpp")
endif ()

list(FILTER MAPBAKER_PLATFORM_SOURCES EXCLUDE REGEX "WinMain[.]cpp$")

add_executable(MapBaker ${MAPBAKER_SOURCES} ${MAPBAKER_CLIENT_SOURCES} ${MAPBAKER_PLATFORM_SOURCES})

set_target_properties(MapBaker PROPERTIES
    OUTPUT_NAME ${MAPBAKER_OUTPUT_NAME}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/Win
)

foreach (config ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
    string(TOUPPER ${config} config)
    set_target_properties(MapBaker PROPERTIES RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_SOURCE_DIR}/bin/Win)
endforeach ()

target_include_directories(MapBaker
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_options(MapBaker PRIVATE "/INCREMENTAL:NO" "/DEBUG")

target_link_libraries(MapBaker
    PRIVATE
        BLPFile
        Base
        BlizzardCore
        ComSat
        ConsoleBase
        Event
        FFX
        Frame
        FrameScript
        lua
        FrameXML
        Gx
        Gxu
        Images
        Model2
        Os
        Scrn
        Services
        ShaderEffect
        Tempest
        SoundEngine
        SteelseriesMouse
        Storm
        WoWCommon
        Battlenet
        Console
        ConsoleDevice
        DB
        Game
        Glue
        Magic
        Minigame
        Net
        Object
        ObjectAlloc
        ObjectMgrClient
        SoundInterface
        UIUtil
        Ui
        WorldClient
        LiquidSystem
        WowServices
        WowSvcs
        zlib
        dbghelp
        ws2_32
)
