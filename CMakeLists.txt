cmake_minimum_required(VERSION 3.21)
project(qtflix VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick Qml QuickControls2 Multimedia Svg Concurrent Network ShaderTools)
qt_standard_project_setup(REQUIRES 6.5)
qt_policy(SET QTP0001 NEW)

file(GLOB_RECURSE QTFLIX_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
file(GLOB_RECURSE QTFLIX_QML CONFIGURE_DEPENDS qml/*.qml)
file(GLOB_RECURSE QTFLIX_ASSETS CONFIGURE_DEPENDS assets/*)

qt_add_executable(qtflix ${QTFLIX_SOURCES})

# Globbed files are absolute; give each a relative resource alias so the qml module can embed them.
foreach(f ${QTFLIX_QML})
    file(RELATIVE_PATH rel ${CMAKE_CURRENT_SOURCE_DIR}/qml ${f})
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${rel})
    get_filename_component(name ${f} NAME_WE)
    if(name STREQUAL "Theme" OR name STREQUAL "Nav" OR name STREQUAL "IconPaths")
        set_source_files_properties(${f} PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
    endif()
endforeach()
foreach(f ${QTFLIX_ASSETS})
    file(RELATIVE_PATH rel ${CMAKE_CURRENT_SOURCE_DIR} ${f})
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${rel})
endforeach()

qt_add_qml_module(qtflix
    URI QtFlix
    VERSION 1.0
    QML_FILES ${QTFLIX_QML}
    RESOURCES ${QTFLIX_ASSETS}
    RESOURCE_PREFIX /
    NO_RESOURCE_TARGET_PATH
    NO_IMPORT_SCAN
)

# Shaders (compiled to .qsb at build time; ShaderEffect loads qrc:/shaders/<name>.qsb)
qt_add_shaders(qtflix "qtflix_shaders"
    PREFIX "/"
    FILES shaders/roundedimage.frag
)

target_include_directories(qtflix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(qtflix PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::Qml Qt6::QuickControls2 Qt6::Multimedia Qt6::Svg Qt6::Concurrent Qt6::Network)

include(GNUInstallDirs)
install(TARGETS qtflix RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/qtflix.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES assets/mark.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME qtflix.svg)

# .deb package: `cpack --config build/CPackConfig.cmake -G DEB` (or ./install.sh --deb)
set(CPACK_PACKAGE_NAME qtflix)
set(CPACK_PACKAGE_CONTACT "elidon555")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Streaming-style player for local video files")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION video)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "ffmpeg, fonts-inter, qml6-module-qtquick, qml6-module-qtquick-controls, \
qml6-module-qtquick-layouts, qml6-module-qtquick-window, qml6-module-qtquick-templates, qml6-module-qtquick-effects, \
qml6-module-qtquick-shapes, qml6-module-qtquick-dialogs, qml6-module-qtqml-workerscript, qml6-module-qtmultimedia, \
qml6-module-qtcore, libqt6svg6")
include(CPack)

# ---------------------------------------------------------------------------
# Lint: `ninja -C build lint`  (qmllint always; clang-tidy / clazy when installed)
# ---------------------------------------------------------------------------
# Warnings only for our own sources (generated qmlcache files would trip -Wunused-parameter)
set_source_files_properties(${QTFLIX_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wshadow;-Wnon-virtual-dtor")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy-21 clang-tidy-20 clang-tidy-19)
find_program(CLAZY_EXE NAMES clazy-standalone)

set(LINT_COMMANDS)
if(CLANG_TIDY_EXE)
    list(APPEND LINT_COMMANDS COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} --quiet ${QTFLIX_SOURCES})
endif()
if(CLAZY_EXE)
    list(APPEND LINT_COMMANDS COMMAND ${CLAZY_EXE} -p ${CMAKE_BINARY_DIR} --checks=level0,level1
         --ignore-included-files ${QTFLIX_SOURCES})
endif()
# qt_add_qml_module already defines `all_qmllint` (qmllint over the module with the right imports).
add_custom_target(lint ${LINT_COMMANDS}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running linters (qmllint; clang-tidy and clazy when installed)"
    VERBATIM)
add_dependencies(lint all_qmllint)
