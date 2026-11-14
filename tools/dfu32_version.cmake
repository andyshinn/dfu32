# SPDX-License-Identifier: MIT
#
# The firmware version: `git describe --tags --dirty --always`, except that
# when several tags point at the commit describe starts from, the highest
# version among them is used.
#
# Plain `git describe` breaks that tie by taking the first lightweight tag in
# name order, so a release tagged on the same commit as its candidates
# (v0.4.0-rc.11, v0.4.0-rc.12, v0.5.0) would report v0.4.0-rc.11. Here a
# release outranks its own pre-releases (v0.5.0 > v0.5.0-rc.3) and numbers
# compare as numbers (rc.10 > rc.9).
#
# The result replaces only the tag at the front, so a build between tags
# still reads v0.5.0-3-gabc1234 and an uncommitted change still ends -dirty.
# Leaves OUT_VAR unset when git can't describe the tree (no git, not a
# checkout), so ESP-IDF falls back to its own default.
#
#   include(tools/dfu32_version.cmake)
#   dfu32_git_version(PROJECT_VER "${CMAKE_CURRENT_LIST_DIR}")
#
# tools/test_dfu32_version.py runs it on throwaway repositories with
# `cmake -DREPO=<dir> -P tools/dfu32_version.cmake`, which prints the result.

function(dfu32_git_version out_var repo_dir)
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        return()
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repo_dir}" describe --tags --dirty --always
                    OUTPUT_VARIABLE desc RESULT_VARIABLE rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT rc EQUAL 0 OR desc STREQUAL "")
        return()
    endif()
    set(version "${desc}")

    # The tag describe started from; fails when there are no tags (desc is a hash).
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repo_dir}" describe --tags --abbrev=0
                    OUTPUT_VARIABLE base RESULT_VARIABLE rc
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(rc EQUAL 0 AND NOT base STREQUAL "")
        # versionsort.suffix=- sorts any "-..." suffix before the bare version.
        execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${repo_dir}" -c versionsort.suffix=-
                                tag --points-at "${base}^{commit}" --sort=-v:refname
                        OUTPUT_VARIABLE tags RESULT_VARIABLE rc
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(rc EQUAL 0 AND NOT tags STREQUAL "")
            string(REPLACE "\n" ";" tags "${tags}")
            list(GET tags 0 best)
            string(LENGTH "${base}" base_len)
            string(SUBSTRING "${desc}" 0 ${base_len} head)
            if(head STREQUAL base)
                string(SUBSTRING "${desc}" ${base_len} -1 rest)
                set(version "${best}${rest}")
            endif()
        endif()
    endif()
    set(${out_var} "${version}" PARENT_SCOPE)
endfunction()

# Script mode, for the tests: cmake -DREPO=<dir> -P tools/dfu32_version.cmake
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    dfu32_git_version(_dfu32_v "${REPO}")
    if(DEFINED _dfu32_v)
        message(NOTICE "${_dfu32_v}")
    endif()
endif()
