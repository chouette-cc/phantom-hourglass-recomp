# Phantom Hourglass Recomp — Native Mod SDK CMake helpers
#
# External mods include this directory:
#   add_subdirectory(path/to/ph-recomp/sdk ph-mods-sdk EXCLUDE_FROM_ALL)
#   ph_add_mod(my_mod SOURCES src/mod.cpp MOD_JSON mod.json RES_DIR res OVERLAY_DIR overlay)

if(NOT TARGET ph_mods_sdk)
  add_library(ph_mods_sdk INTERFACE)
  target_include_directories(ph_mods_sdk INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
  target_compile_features(ph_mods_sdk INTERFACE cxx_std_20)
endif()

function(ph_add_mod TARGET_NAME)
  cmake_parse_arguments(MOD "" "MOD_JSON;RES_DIR;OVERLAY_DIR;TEXTURES_DIR;OUT_DIR" "SOURCES;INCLUDE_DIRS;LINK_LIBRARIES" ${ARGN})
  if(NOT MOD_SOURCES)
    message(FATAL_ERROR "ph_add_mod(${TARGET_NAME}): SOURCES is required")
  endif()
  if(NOT MOD_MOD_JSON)
    message(FATAL_ERROR "ph_add_mod(${TARGET_NAME}): MOD_JSON is required")
  endif()

  add_library(${TARGET_NAME} SHARED ${MOD_SOURCES})
  target_link_libraries(${TARGET_NAME} PRIVATE ph_mods_sdk ${MOD_LINK_LIBRARIES})
  if(MOD_INCLUDE_DIRS)
    target_include_directories(${TARGET_NAME} PRIVATE ${MOD_INCLUDE_DIRS})
  endif()
  set_target_properties(${TARGET_NAME} PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden)

  if(WIN32)
    set(PH_MOD_PLATFORM "windows-x64")
    set(PH_MOD_LIB_NAME "mod.dll")
  elseif(ANDROID)
    set(PH_MOD_PLATFORM "android-arm64")
    set(PH_MOD_LIB_NAME "libmod.so")
  elseif(APPLE)
    set(PH_MOD_PLATFORM "macos-arm64")
    set(PH_MOD_LIB_NAME "libmod.dylib")
  else()
    set(PH_MOD_PLATFORM "linux-x64")
    set(PH_MOD_LIB_NAME "libmod.so")
  endif()

  set(STAGE_DIR "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}_stage")
  if(MOD_OUT_DIR)
    set(BUNDLE_OUT_DIR "${MOD_OUT_DIR}")
  else()
    set(BUNDLE_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
  endif()
  set(BUNDLE_OUT "${BUNDLE_OUT_DIR}/${TARGET_NAME}.phmod")

  set(COPY_CMDS
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${STAGE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${STAGE_DIR}/lib/${PH_MOD_PLATFORM}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE_OUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy "${MOD_MOD_JSON}" "${STAGE_DIR}/mod.json"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TARGET_NAME}>" "${STAGE_DIR}/lib/${PH_MOD_PLATFORM}/${PH_MOD_LIB_NAME}"
  )
  if(MOD_RES_DIR)
    list(APPEND COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOD_RES_DIR}" "${STAGE_DIR}/res")
  endif()
  if(MOD_OVERLAY_DIR)
    list(APPEND COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOD_OVERLAY_DIR}" "${STAGE_DIR}/overlay")
  endif()
  if(MOD_TEXTURES_DIR)
    list(APPEND COPY_CMDS COMMAND ${CMAKE_COMMAND} -E copy_directory "${MOD_TEXTURES_DIR}" "${STAGE_DIR}/textures")
  endif()

  add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    ${COPY_CMDS}
    COMMAND ${CMAKE_COMMAND} -E chdir "${STAGE_DIR}" ${CMAKE_COMMAND} -E tar cf "${BUNDLE_OUT}" --format=zip -- .
    COMMENT "Packaging ${TARGET_NAME}.phmod")
endfunction()
