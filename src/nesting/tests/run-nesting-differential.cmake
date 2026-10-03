# SPDX-License-Identifier: GPL-2.0-or-later

# This driver deliberately runs the baseline and portfolio sequentially. Each
# receives the same positive wall-clock budget without CPU contention between
# the two measurements. The portfolio implementation is responsible for its
# own baseline+experimental concurrency when worker_count=2.
#
# The registered 100/1000/5000 ms CTest gates are opt-in:
#   VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL=1 \
#     ctest --test-dir BUILD -R vacards-nesting-portfolio-differential
# The 60-second-per-case extended gate is independently ignored by default:
#   VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S=1 \
#     ctest --test-dir BUILD -R vacards-nesting-portfolio-differential-60000ms
# The C++ runner's --help output documents equivalent direct CLI commands.

foreach(_required IN ITEMS BASELINE_RUNNER EXPERIMENTAL_RUNNER COMPARATOR_RUNNER OUTPUT_DIR)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

if(DEFINED REQUIRED_ENVIRONMENT AND NOT "${REQUIRED_ENVIRONMENT}" STREQUAL "")
    if(NOT "$ENV{${REQUIRED_ENVIRONMENT}}" STREQUAL "1")
        message(STATUS
            "VACARDS_NESTING_DIFFERENTIAL_SKIPPED: set ${REQUIRED_ENVIRONMENT}=1 to run this opt-in gate")
        return()
    endif()
endif()

if(NOT DEFINED BUDGET_MS)
    set(BUDGET_MS 0)
endif()
if(NOT BUDGET_MS MATCHES "^[0-9]+$")
    message(FATAL_ERROR "BUDGET_MS must be a non-negative integer")
endif()

if(NOT DEFINED BASELINE_WORKER_COUNT)
    set(BASELINE_WORKER_COUNT 1)
endif()
if(NOT DEFINED EXPERIMENTAL_WORKER_COUNT)
    set(EXPERIMENTAL_WORKER_COUNT 1)
endif()
foreach(_worker_variable IN ITEMS BASELINE_WORKER_COUNT EXPERIMENTAL_WORKER_COUNT)
    if(NOT ${_worker_variable} MATCHES "^[1-9][0-9]*$" OR ${${_worker_variable}} GREATER 1024)
        message(FATAL_ERROR "${_worker_variable} must be in [1, 1024]")
    endif()
endforeach()
if((REQUIRE_STRICT_IMPROVEMENT OR REQUIRE_STRICT_ADVERSARIAL_IMPROVEMENT) AND BUDGET_MS EQUAL 0)
    message(FATAL_ERROR "Strict portfolio activation evidence requires a positive BUDGET_MS")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
set(_baseline_report "${OUTPUT_DIR}/baseline.tsv")
set(_experimental_report "${OUTPUT_DIR}/portfolio.tsv")
set(_comparison_report "${OUTPUT_DIR}/comparison.tsv")

function(_vacards_run_report runner output_file backend worker_count part_order)
    execute_process(
        COMMAND "${runner}"
                --benchmark-report "${output_file}"
                --backend-label "${backend}"
                --budget-ms "${BUDGET_MS}"
                --worker-count "${worker_count}"
                --part-order "${part_order}"
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _output
        ERROR_VARIABLE _error)
    if(NOT _status EQUAL 0)
        message(FATAL_ERROR
            "${backend} nesting runner failed (${_status}).\n"
            "stdout:\n${_output}\n"
            "stderr:\n${_error}\n"
            "Partial evidence remains at ${output_file}")
    endif()
endfunction()

_vacards_run_report("${BASELINE_RUNNER}" "${_baseline_report}" baseline
                    "${BASELINE_WORKER_COUNT}" original)
_vacards_run_report("${EXPERIMENTAL_RUNNER}" "${_experimental_report}" portfolio
                    "${EXPERIMENTAL_WORKER_COUNT}" original)

set(_comparison_arguments
    --compare-reports "${_baseline_report}" "${_experimental_report}"
    --comparison-report "${_comparison_report}"
    --expected-baseline-workers "${BASELINE_WORKER_COUNT}"
    --expected-portfolio-workers "${EXPERIMENTAL_WORKER_COUNT}")
if(REQUIRE_IDENTICAL)
    list(APPEND _comparison_arguments --require-identical)
endif()
if(REQUIRE_RANK_EQUIVALENT)
    list(APPEND _comparison_arguments --require-rank-equivalent)
endif()
if(REQUIRE_STRICT_IMPROVEMENT)
    list(APPEND _comparison_arguments --require-strict-improvement)
endif()
if(REQUIRE_STRICT_ADVERSARIAL_IMPROVEMENT)
    list(APPEND _comparison_arguments --require-strict-adversarial-improvement)
endif()
execute_process(
    COMMAND "${COMPARATOR_RUNNER}" ${_comparison_arguments}
    RESULT_VARIABLE _comparison_status
    OUTPUT_VARIABLE _comparison_output
    ERROR_VARIABLE _comparison_error)
set(_gate_failures "")
if(NOT _comparison_status EQUAL 0)
    string(APPEND _gate_failures
        "Nesting differential gate failed (${_comparison_status}).\n"
        "stdout:\n${_comparison_output}\n"
        "stderr:\n${_comparison_error}\n")
endif()

if(CHECK_INPUT_ORDER_PERMUTATION)
    set(_permuted_report "${OUTPUT_DIR}/portfolio-reversed.tsv")
    set(_permutation_comparison_report "${OUTPUT_DIR}/portfolio-input-order.tsv")
    _vacards_run_report("${EXPERIMENTAL_RUNNER}" "${_permuted_report}" portfolio-permuted
                        "${EXPERIMENTAL_WORKER_COUNT}" reversed)
    execute_process(
        COMMAND "${COMPARATOR_RUNNER}"
                --compare-reports "${_experimental_report}" "${_permuted_report}"
                --comparison-report "${_permutation_comparison_report}"
                --expected-baseline-workers "${EXPERIMENTAL_WORKER_COUNT}"
                --expected-portfolio-workers "${EXPERIMENTAL_WORKER_COUNT}"
                --allow-input-order-permutation
                --require-rank-equivalent
        RESULT_VARIABLE _permutation_status
        OUTPUT_VARIABLE _permutation_output
        ERROR_VARIABLE _permutation_error)
    if(NOT _permutation_status EQUAL 0)
        string(APPEND _gate_failures
            "Portfolio input-order permutation gate failed (${_permutation_status}).\n"
            "stdout:\n${_permutation_output}\n"
            "stderr:\n${_permutation_error}\n")
    endif()
    string(APPEND _comparison_output "${_permutation_output}")
endif()

if(NOT "${_gate_failures}" STREQUAL "")
    message(FATAL_ERROR "${_gate_failures}Reports remain at ${OUTPUT_DIR}")
endif()

message(STATUS "${_comparison_output}")
message(STATUS "Nesting differential evidence: ${OUTPUT_DIR}")
