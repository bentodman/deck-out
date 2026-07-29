# Collect git and build metadata for plugin load logging.

include_guard(GLOBAL)

if(NOT PLUGIN_BUILD_NUMBER)
  set(PLUGIN_BUILD_NUMBER "0")
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(PLUGIN_BUILD_TYPE "Unknown")
else()
  set(PLUGIN_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()

find_package(Git QUIET)

set(PLUGIN_GIT_COMMIT "unknown")
set(PLUGIN_GIT_DIRTY "")

if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE _deckout_git_commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )

  if(_deckout_git_commit)
    set(PLUGIN_GIT_COMMIT "${_deckout_git_commit}")
  endif()

  execute_process(
    COMMAND ${GIT_EXECUTABLE} diff-index --quiet HEAD --
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _deckout_git_dirty
    ERROR_QUIET
  )

  if(_deckout_git_dirty)
    set(PLUGIN_GIT_DIRTY "+")
  endif()
endif()

unset(_deckout_git_commit)
unset(_deckout_git_dirty)
