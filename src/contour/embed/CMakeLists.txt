# contour_embed: the terminal display as a shared library, for a Qt Quick host that is not Contour.
#
# SHARED, and everything else linked into it privately, so the host's link line is this library and
# Qt. The public header names only Qt types for the same reason: a host does not compile Contour's
# headers and does not need Contour's C++ standard.
#
# Building it needs position-independent objects throughout, so configure with
# -DCMAKE_POSITION_INDEPENDENT_CODE=ON.

if(NOT CMAKE_POSITION_INDEPENDENT_CODE)
    message(FATAL_ERROR "CONTOUR_EMBED_LIBRARY needs -DCMAKE_POSITION_INDEPENDENT_CODE=ON")
endif()

add_library(contour_embed SHARED Embed.cpp Embed.hpp BuiltinConfig.hpp)
set_target_properties(contour_embed PROPERTIES
    AUTOMOC ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
target_compile_definitions(contour_embed PRIVATE CONTOUR_EMBED_BUILD)
target_include_directories(contour_embed
    PRIVATE $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
    INTERFACE $<INSTALL_INTERFACE:include>)
target_link_libraries(contour_embed
    PUBLIC Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
    PRIVATE contour_core)

# Only contour::embed leaves the library. The preset above hides what Embed.cpp defines and is not
# exported; it does nothing for contour_core and everything under it, which are compiled with
# default visibility for Contour's own executable and would all be exported from here. The version
# script is the list, and check-exports.sh fails the build the moment the library disagrees with
# it -- at link time, because the consumer of this library never runs its tests.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(contour_embed PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/contour_embed.map")
    set_property(TARGET contour_embed APPEND PROPERTY
        LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/contour_embed.map")
    add_custom_command(TARGET contour_embed POST_BUILD
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check-exports.sh"
                $<TARGET_FILE:contour_embed> "${CMAKE_NM}"
        VERBATIM)
    add_test(NAME contour_embed_exports
        COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/check-exports.sh"
                $<TARGET_FILE:contour_embed> "${CMAKE_NM}")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS contour_embed EXPORT ContourEmbedTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT embed)
install(FILES Embed.hpp DESTINATION include/contour/embed COMPONENT embed)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE.txt" DESTINATION share/licenses/contour-embed COMPONENT embed)
install(EXPORT ContourEmbedTargets NAMESPACE Contour:: FILE ContourEmbedTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ContourEmbed COMPONENT embed)
set_target_properties(contour_embed PROPERTIES EXPORT_NAME Embed)

# The package configuration records Qt6_VERSION and refuses a consumer whose Qt is another minor:
# the library reads Qt's private headers. The version file below cannot say this -- SameMinorVersion
# compares Contour's own version, not Qt's.
if(NOT Qt6_VERSION)
    message(FATAL_ERROR "CONTOUR_EMBED_LIBRARY needs Qt6_VERSION, which find_package(Qt6) sets")
endif()
configure_package_config_file(ContourEmbedConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ContourEmbedConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ContourEmbed)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/ContourEmbedConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ContourEmbedConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ContourEmbedConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ContourEmbed COMPONENT embed)
