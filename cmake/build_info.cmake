# Writes build_info.hpp: the app version and the Git commit this build came from.
# Runs on every build and rewrites the file only when its content changes, so an unchanged commit recompiles nothing.
# Inputs: SOURCE_DIR, OUTPUT, VERSION.
set(commit "unknown")
set(dirty 0)
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${SOURCE_DIR}"
    OUTPUT_VARIABLE found OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
  if(NOT failed AND found)
    set(commit "${found}")
    # Tracked files changed since that commit: the build is marked, so it is never mistaken for the commit itself.
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no WORKING_DIRECTORY "${SOURCE_DIR}"
      OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(changes)
      set(dirty 1)
    endif()
  endif()
endif()
string(SUBSTRING "${commit}" 0 7 short)
set(content "#pragma once\n#define OBSREPLAY_VERSION \"${VERSION}\"\n#define OBSREPLAY_COMMIT \"${commit}\"\n#define OBSREPLAY_COMMIT_SHORT \"${short}\"\n#define OBSREPLAY_DIRTY ${dirty}\n")
set(previous "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" previous)
endif()
if(NOT previous STREQUAL content)
  file(WRITE "${OUTPUT}" "${content}")
endif()
