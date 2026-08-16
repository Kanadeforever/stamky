cmake_minimum_required(VERSION 3.25)
project(stamky VERSION 0.9.3 LANGUAGES CXX RC)

add_executable(stamky WIN32
  src/main.cpp
  src/about.cpp
  src/lang.cpp
  src/app.cpp
  src/common.cpp
  src/model.cpp
  src/settings.cpp
  src/shell_utils.cpp
  src/icon_cache.cpp
  src/runtime_cache.cpp
  src/runtime_menu.cpp
  src/manager.cpp
  src/item_editor.cpp
  src/settings_window.cpp
  src/stamky.rc
)

target_include_directories(stamky PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(stamky PRIVATE cxx_std_20)
target_compile_definitions(stamky PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
if(MSVC)
  # app.manifest is already embedded as RT_MANIFEST #1 by stamky.rc.
  # Disable LINK automatic manifest generation to avoid CVT1100 duplicate resource.
  target_compile_options(stamky PRIVATE /W4 /permissive- /EHsc /utf-8 /O2 /Gw /Gy)
  target_link_options(stamky PRIVATE /OPT:REF /OPT:ICF /MANIFEST:NO)
endif()
target_link_libraries(stamky PRIVATE user32 gdi32 shell32 ole32 comctl32 shlwapi psapi advapi32 uxtheme msimg32 propsys)
set_target_properties(stamky PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
