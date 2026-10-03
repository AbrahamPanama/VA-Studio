# SPDX-License-Identifier: GPL-2.0-or-later
# Run with the actual application executable, not a gtest fixture singleton.
# Required: -DINKSCAPE_EXECUTABLE=/absolute/path/to/inkscape
#           -DOUTPUT_DIRECTORY=/absolute/path/to/evidence
#           -DMODE=batch|shell|headless

if(NOT IS_ABSOLUTE "${INKSCAPE_EXECUTABLE}" OR NOT EXISTS "${INKSCAPE_EXECUTABLE}")
    message(FATAL_ERROR "INKSCAPE_EXECUTABLE must name the actual built application")
endif()
if(NOT IS_ABSOLUTE "${OUTPUT_DIRECTORY}")
    message(FATAL_ERROR "OUTPUT_DIRECTORY must be an absolute evidence directory")
endif()
if(NOT MODE MATCHES "^(batch|shell|headless)$")
    message(FATAL_ERROR "MODE must be batch, shell, or headless")
endif()

# Unique evidence and profile directories; preserve failures and never replace
# an installed application, existing output, or the user's preferences.
string(RANDOM LENGTH 12 ALPHABET abcdef0123456789 run_id)
set(evidence "${OUTPUT_DIRECTORY}/immediate-quit-${MODE}-${run_id}")
file(MAKE_DIRECTORY "${evidence}/profile")
file(WRITE "${evidence}/input.svg" [=[
<svg xmlns="http://www.w3.org/2000/svg"
     xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape"
     width="32" height="32" viewBox="0 0 32 32" inkscape:version="1.4"
     inkscape:export-filename="result.png" inkscape:export-xdpi="96">
  <rect id="shape" x="4" y="4" width="16" height="12" fill="#369"/>
</svg>
]=])
file(WRITE "${evidence}/stdin.txt" "quit\n")
set(arguments --export-use-hints --export-type=png --export-area-drawing
              "--app-id-tag=ownerquit${run_id}")
if(MODE STREQUAL "batch")
    list(APPEND arguments --batch-process)
elseif(MODE STREQUAL "shell")
    # shell() requests immediate quit before process_document's auto-export.
    list(APPEND arguments --shell --with-gui)
else()
    # Exercise immediate quit with a registered viewless document too.
    list(APPEND arguments --actions=quit-immediate)
endif()
list(APPEND arguments "${evidence}/input.svg")
file(WRITE "${evidence}/invocation.txt" "${INKSCAPE_EXECUTABLE}\n${arguments}\nG_DEBUG=fatal-criticals\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "G_DEBUG=fatal-criticals"
            "INKSCAPE_PROFILE_DIR=${evidence}/profile"
            "${INKSCAPE_EXECUTABLE}" ${arguments}
    WORKING_DIRECTORY "${evidence}"
    INPUT_FILE "${evidence}/stdin.txt"
    OUTPUT_FILE "${evidence}/stdout.log"
    ERROR_FILE "${evidence}/stderr.log"
    RESULT_VARIABLE status
    TIMEOUT 60
)
file(WRITE "${evidence}/status.txt" "${status}\n")
message(STATUS "Immediate-quit evidence: ${evidence}")
if(NOT "${status}" STREQUAL "0")
    message(FATAL_ERROR "Application did not exit normally: ${status}; see preserved evidence")
endif()
file(READ "${evidence}/stderr.log" stderr)
if(stderr MATCHES "desktops still in list|CRITICAL|Window vector not empty|desktop not found|document not in map")
    message(FATAL_ERROR "Application teardown diagnostic; see ${evidence}/stderr.log")
endif()
if(stderr MATCHES "[Uu]nknown action|could not find action for")
    message(FATAL_ERROR "Requested quit action was not exercised; see ${evidence}/stderr.log")
endif()
if(NOT MODE STREQUAL "headless" AND stderr MATCHES "No GUI available")
    message(FATAL_ERROR "GUI smoke was not exercised: no display available")
endif()
if(NOT EXISTS "${evidence}/result.png")
    message(FATAL_ERROR "Automatic PNG export did not finish before application exit")
endif()
file(READ "${evidence}/result.png" signature LIMIT 8 HEX)
if(NOT signature STREQUAL "89504e470d0a1a0a")
    message(FATAL_ERROR "Export result is not a PNG")
endif()
# IHDR must describe the 16 x 12 drawing at the fixture's 96 DPI hint,
# not an empty/truncated signature or the full 32 x 32 page.
file(READ "${evidence}/result.png" dimensions OFFSET 12 LIMIT 12 HEX)
if(NOT dimensions STREQUAL "49484452000000100000000c")
    message(FATAL_ERROR "PNG dimensions do not match the exported drawing")
endif()
message(STATUS "${MODE}: PNG exported and actual application exited normally")
