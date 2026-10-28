# Runs one test script of kerf_headless with a folder of its own that starts out empty: the takes
# that the Record screen saves go there, so they are numbered the same way every time.
#
#   cmake -DHEADLESS=<kerf_headless> -DSCRIPT=<script.txt> -DFOLDER=<folder> [-DOPTIONS=--fake-input]
#         [-DSCRIPT2=<second script.txt>] [-DMAKE_FOLDERS=<names>] [-DFAILS_WITH=<regular expression>]
#         [-DEMULATOR=<program that runs it, when cross-compiled>] -P run_in_empty_folder.cmake
#
# SCRIPT2 is run after SCRIPT, in the same folder, as a program of its own: a second run of Kerf that
# finds on the disk what the first left (the projects' test starts Kerf twice).
#
# MAKE_FOLDERS is a list of empty folders to make in it before the run (the projects' test puts a
# folder where a part file of a project goes, so that the disk "will not take" the save).
#
# FAILS_WITH turns the test around: the script has a wrong check in it on purpose, the run has to
# fail, and what the program printed has to match the regular expression. That is how the checks
# of the script runner itself are tested (a check that could never fail would let everything pass).
#
# Fails when a script fails. The folder is left behind when it does (to look at what was saved),
# and removed when they pass.

if(NOT HEADLESS OR NOT SCRIPT OR NOT FOLDER)
    message(FATAL_ERROR "usage: cmake -DHEADLESS=... -DSCRIPT=... -DFOLDER=... [-DOPTIONS=...] -P run_in_empty_folder.cmake")
endif()

file(REMOVE_RECURSE "${FOLDER}")
file(MAKE_DIRECTORY "${FOLDER}")

foreach(_name IN LISTS MAKE_FOLDERS)
    file(MAKE_DIRECTORY "${FOLDER}/${_name}")
endforeach()

if(FAILS_WITH)
    execute_process(
        COMMAND ${EMULATOR} "${HEADLESS}" --script "${SCRIPT}" --start-dir "${FOLDER}" ${OPTIONS}
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)

    if(_result EQUAL 0)
        message(FATAL_ERROR "kerf_headless passed ${SCRIPT}, which has a wrong check in it on purpose")
    endif()

    if(NOT "${_out}${_err}" MATCHES "${FAILS_WITH}")
        message(FATAL_ERROR "kerf_headless failed ${SCRIPT} but not the way it should (wanted: ${FAILS_WITH}); it printed:\n${_out}${_err}")
    endif()

    file(REMOVE_RECURSE "${FOLDER}")
    return()
endif()

execute_process(
    COMMAND ${EMULATOR} "${HEADLESS}" --script "${SCRIPT}" --start-dir "${FOLDER}" ${OPTIONS}
    RESULT_VARIABLE _result)

if(NOT _result EQUAL 0)
    message(FATAL_ERROR "kerf_headless failed (${_result}) for ${SCRIPT}; the folder ${FOLDER} was kept")
endif()

if(SCRIPT2)
    execute_process(
        COMMAND ${EMULATOR} "${HEADLESS}" --script "${SCRIPT2}" --start-dir "${FOLDER}" ${OPTIONS}
        RESULT_VARIABLE _result2)

    if(NOT _result2 EQUAL 0)
        message(FATAL_ERROR "kerf_headless failed (${_result2}) for ${SCRIPT2} (the second run); the folder ${FOLDER} was kept")
    endif()
endif()

file(REMOVE_RECURSE "${FOLDER}")
