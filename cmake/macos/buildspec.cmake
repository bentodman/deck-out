# CMake macOS build dependencies module

include_guard(GLOBAL)

include(buildspec_common)

# _check_dependencies_macos: Set up macOS slice for _check_dependencies
function(_check_dependencies_macos)
  set(arch universal)
  set(platform macos)

  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" buildspec)

  set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
  set(prebuilt_filename "macos-deps-VERSION-ARCH_REVISION.tar.xz")
  set(prebuilt_destination "obs-deps-VERSION-ARCH")
  set(qt6_filename "macos-deps-qt6-VERSION-ARCH-REVISION.tar.xz")
  set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
  set(obs-studio_filename "VERSION.tar.gz")
  set(obs-studio_destination "obs-studio-VERSION")
  if(DEFINED OBS_DEV_PREFIX AND EXISTS "${OBS_DEV_PREFIX}/Frameworks/libobs.framework/Resources/cmake/libobsConfig.cmake")
    set(dependencies_list prebuilt qt6)
    message(STATUS "Using local OBS development install at ${OBS_DEV_PREFIX}")
  else()
    set(dependencies_list prebuilt qt6 obs-studio)
  endif()

  _check_dependencies()

  if(DEFINED OBS_DEV_PREFIX AND EXISTS "${OBS_DEV_PREFIX}/Frameworks/libobs.framework/Resources/cmake/libobsConfig.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${OBS_DEV_PREFIX}")
    set(libobs_DIR "${OBS_DEV_PREFIX}/Frameworks/libobs.framework/Resources/cmake" CACHE PATH "libobs package directory" FORCE)
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)

    if(DEFINED ENV{OBS_SRC})
      set(_deckout_obs_src "$ENV{OBS_SRC}")
    else()
      set(_deckout_obs_src "$ENV{HOME}/obs-dev/obs-studio")
    endif()
    set(_deckout_fe_api_dir "${_deckout_obs_src}/build_macos/frontend/api")
    if(EXISTS "${_deckout_fe_api_dir}/obs-frontend-apiConfig.cmake")
      set(obs-frontend-api_DIR "${_deckout_fe_api_dir}" CACHE PATH "obs-frontend-api package directory" FORCE)
      list(APPEND CMAKE_PREFIX_PATH "${_deckout_fe_api_dir}")
      set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} CACHE PATH "CMake prefix search path" FORCE)
    endif()
  endif()

  execute_process(
    COMMAND "xattr" -r -d com.apple.quarantine "${dependencies_dir}"
    RESULT_VARIABLE result
    COMMAND_ERROR_IS_FATAL ANY
  )

  list(APPEND CMAKE_FRAMEWORK_PATH "${dependencies_dir}/Frameworks")
  if(DEFINED OBS_DEV_PREFIX AND EXISTS "${OBS_DEV_PREFIX}/Frameworks")
    list(APPEND CMAKE_FRAMEWORK_PATH "${OBS_DEV_PREFIX}/Frameworks")
  endif()
  set(CMAKE_FRAMEWORK_PATH ${CMAKE_FRAMEWORK_PATH} PARENT_SCOPE)
endfunction()

_check_dependencies_macos()
