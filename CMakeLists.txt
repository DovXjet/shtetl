cmake_minimum_required(VERSION 3.21)
project(shtetl CXX)
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MINGW AND NOT EXISTS "${MINGW_DEPS_PREFIX}/lib/cmake/vsg/vsgConfig.cmake"
   AND NOT EXISTS "${MINGW_DEPS_PREFIX}/lib64/cmake/vsg/vsgConfig.cmake")
  # Cross build: vsg/vsgXchange/assimp aren't packaged for MinGW; build them once.
  message(STATUS "Building MinGW dependencies (tools/build-mingw-deps.sh)...")
  execute_process(COMMAND ${CMAKE_COMMAND} -E env MINGW_DEPS_PREFIX=${MINGW_DEPS_PREFIX}
                          ${CMAKE_SOURCE_DIR}/tools/build-mingw-deps.sh
                  RESULT_VARIABLE _deps_rc)
  if(NOT _deps_rc EQUAL 0)
    message(FATAL_ERROR "tools/build-mingw-deps.sh failed")
  endif()
endif()
find_package(vsg CONFIG REQUIRED)
find_package(vsgXchange CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)
# libzip reads the zip of a 3MF file (ZIP64 too, which QZipReader cannot).
find_package(libzip CONFIG REQUIRED)
find_package(OpenMP QUIET)
# OpenCASCADE reads STEP and IGES files; without it those are not opened.
find_package(OpenCASCADE CONFIG QUIET)

# The system-wide vsgQt install (/usr/local) was built against Qt5, so build
# vsgQt from source against Qt6 and the system vsg - same as RobotMotion.
include(FetchContent)
set(QT_PACKAGE_NAME Qt6 CACHE STRING "" FORCE)
# EXCLUDE_FROM_ALL keeps vsgQt's example apps and install rules out of the
# build/install (shtetl still builds the vsgQt library it links). Needs 3.28+.
set(SHTETL_VSGQT_EXCLUDE "")
if(NOT CMAKE_VERSION VERSION_LESS 3.28)
  set(SHTETL_VSGQT_EXCLUDE EXCLUDE_FROM_ALL)
endif()
FetchContent_Declare(vsgQt
  GIT_REPOSITORY https://github.com/vsg-dev/vsgQt.git
  GIT_TAG v0.4.0
  GIT_SHALLOW TRUE
  ${SHTETL_VSGQT_EXCLUDE}
)
FetchContent_MakeAvailable(vsgQt)

# QtAwesome (MIT) themes the icons of the Open dialog. EXCLUDE_FROM_ALL: it
# installs its own library and export set, which has no business in our prefix.
add_subdirectory(third_party/QtAwesome EXCLUDE_FROM_ALL)

add_executable(shtetl
  src/main.cpp
  src/Logging.cpp
  src/MainWindow.cpp
  src/SceneSettingsDialog.cpp
  src/Viewport3D.cpp
  src/AnimationPlayer.cpp
  src/AnimationBar.cpp
  src/CubeGizmo.cpp
  src/ModelFormats.cpp
  src/CompactMesh.cpp
  src/EdgeMesh.cpp
  src/AmbientOcclusion.cpp
  src/Mesh3mf.cpp
  src/ThumbnailRenderer.cpp
  src/ThumbnailService.cpp
  src/ThumbnailBrowser.cpp
  src/OpenFileDialog.cpp
  src/Preferences.cpp
)
target_include_directories(shtetl PRIVATE src)
if(WIN32)
  # UTF-8 active code page, so narrow (std::string) file paths may be non-ASCII.
  target_sources(shtetl PRIVATE resources/shtetl.manifest resources/shtetl.rc)
  # GUI subsystem: no console window. Keep the plain main() entry point
  # instead of requiring WinMain.
  set_target_properties(shtetl PROPERTIES WIN32_EXECUTABLE TRUE)
  if(MSVC)
    target_link_options(shtetl PRIVATE /ENTRY:mainCRTStartup)
  else()
    target_link_options(shtetl PRIVATE -Wl,-subsystem,windows -Wl,-e,mainCRTStartup)
  endif()
endif()

# Orientation cube meshes (from RobotMotion/XjetStudio), embedded so the
# binary needs no resource directory at run time.
file(GLOB CUBE_GIZMO_STL CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} resources/cube-gizmo/*.stl)
qt_add_resources(shtetl cube_gizmo PREFIX / BASE resources FILES ${CUBE_GIZMO_STL})
# The shaders of the compact meshes, compiled by VSG at run time.
set(SHTETL_SHADER_FILES
            resources/shaders/compact_mesh.vert resources/shaders/compact_mesh.frag
            resources/shaders/compact_mesh_gbuffer.frag resources/shaders/ao_fullscreen.vert resources/shaders/ao_ssao.frag resources/shaders/ao_blur.frag
            resources/shaders/ao_composite.frag resources/shaders/ao_fxaa.frag resources/shaders/gradient_background.frag
            resources/shaders/pbr_environment.glsl resources/shaders/compact_pbr_common.glsl resources/shaders/compact_pbr.frag resources/shaders/compact_pbr_gbuffer.frag
            resources/shaders/edge_lines.vert resources/shaders/edge_lines.frag resources/shaders/edge_lines_gbuffer.frag)
qt_add_resources(shtetl shaders PREFIX / BASE resources FILES ${SHTETL_SHADER_FILES})
# The application logo (window icon; the .ico is embedded by resources/shtetl.rc).
qt_add_resources(shtetl icons PREFIX / BASE resources FILES resources/icons/shtetl.png resources/icons/shtetl-about.png)

# libassimp (via vsgXchange) bundles the kuba/zip library, which exports
# zip_open/zip_close/... under the same names as libzip with an incompatible
# API (see XjetStudio's src/CMakeLists.txt). Linking libzip first, and keeping
# it NEEDED, makes its symbols win at run time.
if(UNIX AND NOT APPLE)
  target_link_options(shtetl PRIVATE -Wl,--no-as-needed)
endif()
target_link_libraries(shtetl PRIVATE libzip::zip vsg::vsg vsgXchange::vsgXchange vsgQt::vsgQt QtAwesome spdlog::spdlog Qt6::Core Qt6::Gui Qt6::Widgets)
if(OpenMP_CXX_FOUND)
  target_link_libraries(shtetl PRIVATE OpenMP::OpenMP_CXX)
endif()
if(OpenCASCADE_FOUND)
  message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION}: STEP and IGES import enabled")
  target_sources(shtetl PRIVATE src/CadReader.cpp)
  target_compile_definitions(shtetl PRIVATE SHTETL_HAS_CAD)
  target_include_directories(shtetl PRIVATE ${OpenCASCADE_INCLUDE_DIR})
  target_link_libraries(shtetl PRIVATE TKDESTEP TKDEIGES TKBRep TKMesh TKGeomBase TKG3d TKMath TKTopAlgo TKGeomAlgo TKernel)
else()
  message(STATUS "OpenCASCADE not found: STEP and IGES import disabled")
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(shtetl PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

# A headless thumbnail renderer for checking changes to the thumbnails without
# running shtetl: cmake --build <dir> --target thumbtool. Not part of "all".
add_executable(thumbtool EXCLUDE_FROM_ALL
  tools/thumbtool.cpp
  src/ThumbnailRenderer.cpp
  src/CompactMesh.cpp
  src/Mesh3mf.cpp
  src/ModelFormats.cpp
)
target_include_directories(thumbtool PRIVATE src)
qt_add_resources(thumbtool shaders PREFIX / BASE resources FILES ${SHTETL_SHADER_FILES})
if(UNIX AND NOT APPLE)
  target_link_options(thumbtool PRIVATE -Wl,--no-as-needed)
endif()
target_link_libraries(thumbtool PRIVATE libzip::zip vsg::vsg vsgXchange::vsgXchange spdlog::spdlog Qt6::Core Qt6::Gui)
if(OpenMP_CXX_FOUND)
  target_link_libraries(thumbtool PRIVATE OpenMP::OpenMP_CXX)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(thumbtool PRIVATE pthread ${CMAKE_DL_LIBS})
endif()

# Version / commit info shown in Help > About (taken when CMake configures).
file(READ ${CMAKE_SOURCE_DIR}/vcpkg.json _vcpkg_json)
string(JSON SHTETL_VERSION GET "${_vcpkg_json}" version)
set(SHTETL_COMMIT_ID unknown)
set(SHTETL_COMMIT_DATE unknown)
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --format=%H
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE _id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} log -1 --format=%ci
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                  OUTPUT_VARIABLE _date OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_id)
    set(SHTETL_COMMIT_ID ${_id})
    set(SHTETL_COMMIT_DATE ${_date})
  endif()
endif()

target_compile_definitions(shtetl PRIVATE
  SHTETL_VERSION="${SHTETL_VERSION}"
  SHTETL_COMMIT_ID="${SHTETL_COMMIT_ID}"
  SHTETL_COMMIT_DATE="${SHTETL_COMMIT_DATE}"
)

install(TARGETS shtetl RUNTIME DESTINATION bin)

if(WIN32)
  # Qt needs its platform plugin next to the exe (vcpkg's applocal step only
  # copies DLLs linked at build time, not runtime-loaded plugins).
  if(MINGW)
    # Cross build with Fedora's mingw64-qt6-qtbase: <sysroot>/lib/qt6/plugins.
    find_file(SHTETL_QWINDOWS_PLUGIN qwindows.dll
      PATHS ${CMAKE_FIND_ROOT_PATH} ${CMAKE_PREFIX_PATH}
      PATH_SUFFIXES lib/qt6/plugins/platforms plugins/platforms
      NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
  else()
    find_file(SHTETL_QWINDOWS_PLUGIN qwindows.dll
      PATHS ${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms
      NO_DEFAULT_PATH)
  endif()
  add_custom_command(TARGET shtetl POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:shtetl>/platforms
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SHTETL_QWINDOWS_PLUGIN} $<TARGET_FILE_DIR:shtetl>/platforms/
  )
  # Installed layout: bin/ holds shtetl.exe, every runtime DLL that vcpkg's
  # applocal step put in the build tree and Qt's platforms/ plugin dir. The
  # NSIS installer (Installers/) packages this prefix.
  install(CODE "
    file(GLOB _shtetl_dlls \"${CMAKE_BINARY_DIR}/*.dll\")
    file(INSTALL \${_shtetl_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin\")
    file(INSTALL \"${CMAKE_BINARY_DIR}/platforms/qwindows.dll\" DESTINATION \"\${CMAKE_INSTALL_PREFIX}/bin/platforms\")
  ")
  # App-local MSVC runtime, so no separate vc_redist install is needed.
  # (MinGW cross builds: tools/build-mingw.sh copies the DLLs instead.)
  if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
    include(InstallRequiredSystemLibraries)
  endif()
endif()
