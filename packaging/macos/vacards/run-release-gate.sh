#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later

set -eu

usage()
{
    cat >&2 <<'EOF'
usage:
  run-release-gate.sh --quick BUILD_DIR
  run-release-gate.sh --full BUILD_DIR FRESH_INSTALL_PREFIX

--quick builds and runs the exact critical and native suites. It never creates a
release attestation. --full additionally runs actionable and portfolio suites, installs into an
exclusively reserved new absolute prefix and writes the commit-bound attestation
required by the bundler. Both modes require a freshly configured Ninja tree;
preserve failed runs and use a new tree for another attempt.
Production authentication/acceptance transport is not provisioned: both modes
currently fail before qualification work. Use cmake/ctest separately for development.
EOF
    exit 2
}

[ "$#" -ge 2 ] || usage
mode=$1
build_arg=$2
case "$mode" in
    --quick) [ "$#" -eq 2 ] || usage ;;
    --full)  [ "$#" -eq 3 ] || usage ;;
    *) usage ;;
esac

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
build_dir=$(CDPATH= cd -- "$build_arg" && pwd)
cache=$build_dir/CMakeCache.txt
jobs=${VACARDS_GATE_JOBS:-2}
evidence_dir=$build_dir/vacards-release-evidence
version_file=$script_dir/../../vacards/VERSION.env
version_tool=$script_dir/vacards-version.sh
dependency_tool=$script_dir/vacards-dependencies.sh
dependency_manifest=$source_root/VACARDS-DEPENDENCIES.env
resolved_features=$build_dir/VACARDS-CMAKE-FEATURES.env
packaging_provenance=$script_dir/../../vacards/packaging-provenance.py
gate_run=$script_dir/../../vacards/gate-run.py
test_results=$script_dir/../../vacards/test-results.py
base_version=$("$version_tool" --base)
build_number=$("$version_tool" --build)
release_version=$("$version_tool" --release)
source_baseline_commit=$("$dependency_tool" --get source_baseline_commit)
expected_libcdr_version=$("$dependency_tool" --get libcdr_pkgconfig_version)
expected_libcdr_commit=$("$dependency_tool" --get libcdr_commit)
expected_deployment_target=$("$dependency_tool" --get macos_deployment_target)
required_cmake_features=$("$dependency_tool" --get required_cmake_features)
disabled_cmake_features=$("$dependency_tool" --get disabled_cmake_features)

fail()
{
    echo "VACards release gate failed: $*" >&2
    exit 1
}

cache_value()
{
    key=$1
    sed -n "s/^${key}:[^=]*=//p" "$cache" | tail -n 1
}

manifest_value()
{
    file=$1
    key=$2
    awk -F= -v key="$key" '$1 == key {sub(/^[^=]*=/, ""); print; exit}' "$file"
}

require_authenticated_baseline()
{
    # No candidate-written receipt, ambient path or flag supplies this authority.
    # Provision the independent verifier/protected identity handoff through the
    # release owners before replacing this guard. Fixtures replace only copies.
    fail "production baseline authentication is not provisioned; owners must supply the approved tracked WP-00.6 baseline/signature, independent verifier and protected identity transport; use separate development cmake/ctest commands"
}

require_external_acceptance()
{
    fail "production acceptance verification is not provisioned; owners must supply trusted WP-08 assertion/randomized/per-case portfolio evidence and its verifier/transport before a fresh integrated full gate can authorize packaging"
}

verify_run()
{
    python3 "$gate_run" verify --build "$build_dir" ||
        fail "gate run verification failed ($1); preserve this run and configure a new tree"
}

run_suite()
(
    suite=$1
    # Closed selectors, isolated opt-ins, and one invocation per suite/run.
    unset VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S
    case "$suite" in
        critical)
            stem=critical-tests; set -- --label-regex vacards-critical ;;
        native_nesting)
            stem=native-nesting-tests
            set -- -R '^(vacards-nesting-rust-unit|vacards-nesting-randomized-feasibility|vacards-nesting-abi-smoke-c|vacards-nesting-abi-smoke-cpp|vacards-nesting-job-contract-c|vacards-nesting-cpp-contract|vacards-nesting-differential-selfcheck)$' ;;
        actionable)
            stem=actionable-tests
            set -- --label-exclude '^(upstream-platform-sensitive|vacards-nesting-opt-in)$' ;;
        portfolio_short)
            stem=portfolio-differential
            export VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL=1
            set -- -R '^vacards-nesting-portfolio-differential-(100|1000|5000)ms$' ;;
        portfolio_60s)
            stem=portfolio-differential-60s
            export VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S=1
            set -- -R '^vacards-nesting-portfolio-differential-60000ms$' ;;
        *) fail "unknown scheduled suite: $suite" ;;
    esac
    export INKSCAPE_APP_ID_TAG="$test_app_id"
    if [ "$(uname -s)" = "Darwin" ]; then
        export DYLD_LIBRARY_PATH="$cairo_prefix/lib" GSETTINGS_SCHEMA_DIR="$tested_schemas"
    fi
    verify_run "before $suite enumeration"
    # No clobber: preserve even partial enumeration/log output on failure.
    set -C
    ctest --test-dir "$build_dir" --show-only=json-v1 "$@" \
        >"$evidence_dir/$stem.json" 2>"$evidence_dir/$stem.enumeration.log" ||
        fail "$suite enumeration failed; preserve partial evidence"
    verify_run "before $suite freeze"
    python3 "$test_results" freeze-selection --ledger "$build_dir/VACARDS-GATE-RUN.json" \
        --baseline-identity "$authenticated_identity_file" --suite "$suite" \
        --enumeration "$evidence_dir/$stem.json" || fail "$suite selection rejected"
    vacards_test_exit=0
    ctest --test-dir "$build_dir" --output-on-failure --parallel 1 --no-tests=error "$@" \
        --output-junit "$evidence_dir/$stem.xml" >"$evidence_dir/$stem.log" 2>&1 || vacards_test_exit=$?
    cat "$evidence_dir/$stem.log"
    verify_run "after $suite tests / before result verification and before install"
    vacards_verify_exit=0
    python3 "$test_results" verify-result --ledger "$build_dir/VACARDS-GATE-RUN.json" \
        --baseline-identity "$authenticated_identity_file" --suite "$suite" \
        --ctest-exit-code "$vacards_test_exit" || vacards_verify_exit=$?
    [ "$vacards_test_exit" -eq 0 ] && [ "$vacards_verify_exit" -eq 0 ] ||
        fail "$suite tests/result rejected (CTest exit $vacards_test_exit); failure evidence retained"
)

close_suites()
{
    verify_run "before suite closure"
    ( set -C
      python3 "$test_results" verify-closure --ledger "$build_dir/VACARDS-GATE-RUN.json" \
          --baseline-identity "$authenticated_identity_file" >"$evidence_dir/test-suite-closure.json"
    ) || fail "suite closure rejected; preserve all records"
    verify_run "after suite closure"
}

[ -f "$cache" ] || fail "not a configured build: $build_dir"
require_authenticated_baseline
[ -f "$gate_run" ] || fail "missing qualification reservation helper: $gate_run"
[ -f "$test_results" ] || fail "missing exact test result helper: $test_results"
[ "$(git -C "$source_root" cat-file -t "$source_baseline_commit" 2>/dev/null || true)" = "commit" ] ||
    fail "required source baseline $source_baseline_commit is not present"
git -C "$source_root" merge-base --is-ancestor "$source_baseline_commit" HEAD ||
    fail "source HEAD does not contain required VACards baseline $source_baseline_commit"
[ "$(cache_value CMAKE_HOME_DIRECTORY)" = "$source_root" ] ||
    fail "build was configured from a different source tree: $(cache_value CMAKE_HOME_DIRECTORY)"
[ "$(cache_value BUILD_TESTING)" = "ON" ] || fail "BUILD_TESTING must be ON"
for feature in VACARDS_NESTING_BUILD_PHASE0_TESTS VACARDS_NESTING_BUILD_PHASE2_TESTS; do
    [ "$(cache_value "$feature")" = "ON" ] || fail "$feature must be ON"
done
[ "$(cache_value WITH_LIBCDR)" = "ON" ] || fail "WITH_LIBCDR must be ON"
[ "$(cache_value VACARDS_REQUIRE_MODERN_CDR)" = "ON" ] ||
    fail "VACARDS_REQUIRE_MODERN_CDR must be ON"
[ "$(cache_value VACARDS_RESOLVED_LIBCDR_VERSION)" = "$expected_libcdr_version" ] ||
    fail "build resolved libcdr $(cache_value VACARDS_RESOLVED_LIBCDR_VERSION); expected $expected_libcdr_version"
[ -f "$resolved_features" ] || fail "build is missing resolved feature evidence"
[ "$(manifest_value "$resolved_features" format)" = "1" ] ||
    fail "unsupported resolved feature evidence"
for feature in $(printf '%s' "$required_cmake_features" | tr ',' ' '); do
    [ "$(manifest_value "$resolved_features" "$feature")" = "ON" ] ||
        fail "required release feature $feature is not ON"
done
for feature in $(printf '%s' "$disabled_cmake_features" | tr ',' ' '); do
    [ "$(manifest_value "$resolved_features" "$feature")" = "OFF" ] ||
        fail "release policy requires $feature to be explicitly OFF"
done
case "$(cache_value CMAKE_BUILD_TYPE)" in
    Release|RelWithDebInfo) ;;
    *) fail "CMAKE_BUILD_TYPE must be Release or RelWithDebInfo" ;;
esac
if [ "$(uname -s)" = "Darwin" ] && [ "$(cache_value TESTS_WITH_ASAN)" != "OFF" ]; then
    fail "TESTS_WITH_ASAN must be OFF for the deterministic macOS release gate"
fi
if [ "$(uname -s)" = "Darwin" ]; then
    [ "$(cache_value VACARDS_REQUIRE_PATCHED_CAIRO)" = "ON" ] ||
        fail "VACARDS_REQUIRE_PATCHED_CAIRO must be ON"
    [ "$(cache_value CMAKE_OSX_DEPLOYMENT_TARGET)" = "$expected_deployment_target" ] ||
        fail "CMAKE_OSX_DEPLOYMENT_TARGET must be $expected_deployment_target"
fi
case "$jobs" in
    ''|*[!0-9]*) fail "VACARDS_GATE_JOBS must be a positive integer" ;;
    0) fail "VACARDS_GATE_JOBS must be greater than zero" ;;
esac
# CTest assigns a separate INKSCAPE_PROFILE_DIR to every application test.
# Preserve those per-test values (some tests intentionally unset the variable)
# and add a gate-only application identity. Run serially so no two invocations
# can relay open events through that identity.
test_app_id=vacards-release-gate-$(git -C "$source_root" rev-parse --short=10 HEAD)

"$dependency_tool" --validate >/dev/null
libcdr_prefix=$(cache_value VACARDS_RESOLVED_LIBCDR_PREFIX)
[ -d "$libcdr_prefix" ] || fail "resolved libcdr prefix is missing: $libcdr_prefix"
libcdr_prefix=$(CDPATH= cd -- "$libcdr_prefix" && pwd)
"$script_dir/verify-vacards-libcdr.sh" "$libcdr_prefix"
libcdr_manifest=$libcdr_prefix/VACARDS-LIBCDR.env
[ "$(manifest_value "$libcdr_manifest" source_commit)" = "$expected_libcdr_commit" ] ||
    fail "libcdr provenance does not identify $expected_libcdr_commit"

if [ "$(uname -s)" = "Darwin" ]; then
    # Tests and runtime probes must use the same loader selection.
    for variable in $(env | sed -n 's/^\(DYLD_[A-Za-z0-9_]*\)=.*/\1/p'); do
        unset "$variable"
    done
    cairo_prefix=$(cache_value VACARDS_RESOLVED_CAIRO_PREFIX)
    [ -d "$cairo_prefix" ] || fail "resolved Cairo prefix is missing: $cairo_prefix"
    cairo_prefix=$(CDPATH= cd -- "$cairo_prefix" && pwd)
    "$script_dir/verify-vacards-cairo-prefix.sh" "$cairo_prefix"
    export DYLD_LIBRARY_PATH="$cairo_prefix/lib"
fi

git -C "$source_root" diff --check || fail "git diff --check found whitespace errors"
tracked_changes=$(git -C "$source_root" status --porcelain --untracked-files=no)
if [ -n "$tracked_changes" ]; then
    if [ "$mode" = "--full" ] || [ "${VACARDS_GATE_ALLOW_DIRTY:-0}" != "1" ]; then
        printf '%s\n' "$tracked_changes" >&2
        fail "tracked source changes must be committed before this gate"
    fi
    echo "WARNING: quick gate is running against explicitly allowed dirty source" >&2
fi

submodule_status=$(git -C "$source_root" submodule status --recursive)
bad_submodules=$(printf '%s\n' "$submodule_status" | grep -E '^[+-U]' || true)
[ -z "$bad_submodules" ] || {
    printf '%s\n' "$bad_submodules" >&2
    fail "submodules are missing or differ from the source commit"
}
empty_submodules=$(git -C "$source_root" submodule foreach --quiet --recursive '
    content=$(find . -mindepth 1 -maxdepth 1 ! -name .git -print -quit)
    test -n "$content" || printf "%s\n" "$name"
')
[ -z "$empty_submodules" ] || {
    printf '%s\n' "$empty_submodules" >&2
    fail "submodule worktrees are empty despite having Git metadata"
}
# No evidence, build, or install writes precede this exclusive reservation.
# The helper owns freshness/path/identity checks; never mkdir/rm/reuse its
# outputs here. Exact test inventory/authentication remain separate contracts.
if [ "$mode" = "--full" ]; then
    install_arg=$3
    case "$install_arg" in
        /*) ;;
        *) fail "full install prefix must be absolute: $install_arg" ;;
    esac
    [ -d "$(dirname -- "$install_arg")" ] || fail "full install parent must already exist"
    python3 "$gate_run" claim --source "$source_root" --build "$build_dir" \
        --mode full --install "$install_arg" || fail "could not reserve fresh full gate run"
    install_prefix=$(CDPATH= cd -- "$install_arg" && pwd -P)
else
    python3 "$gate_run" claim --source "$source_root" --build "$build_dir" \
        --mode quick || fail "could not reserve fresh quick gate run"
fi
cp "$version_file" "$evidence_dir/VACARDS-VERSION.env"
cp "$dependency_manifest" "$evidence_dir/VACARDS-DEPENDENCIES.env"
cp "$libcdr_manifest" "$evidence_dir/VACARDS-LIBCDR.env"
cp "$resolved_features" "$evidence_dir/VACARDS-CMAKE-FEATURES.env"
submodule_status_file=$evidence_dir/source-submodules.txt
printf '%s' "$submodule_status" > "$submodule_status_file"
[ -z "$submodule_status" ] || printf '\n' >> "$submodule_status_file"
submodules_sha256=$(shasum -a 256 "$submodule_status_file" | awk '{print $1}')
cmake --build "$build_dir" --parallel "$jobs" --target build-vacards-critical \
    vacards_nesting_abi_smoke_c vacards_nesting_abi_smoke_cpp \
    vacards_nesting_job_contract_c vacards_nesting_cpp_contract_test || \
    fail "critical test executables did not build"
verify_run "after critical build"
if [ "$(uname -s)" = "Darwin" ]; then
    input_run=$(mktemp -d "$evidence_dir/packaging-inputs.XXXXXX")
    packaging_inputs=$input_run/VACARDS-PACKAGING-INPUTS.json
    tested_schemas=$input_run/gtk-schemas
    python3 "$packaging_provenance" capture --cache "$cache" \
        --executable "$build_dir/bin/inkscape" --cairo-prefix "$cairo_prefix" \
        --libcdr-prefix "$libcdr_prefix" --schema-dir "$tested_schemas" \
        --output "$packaging_inputs" || fail "could not freeze packaging inputs"

fi
run_suite critical
run_suite native_nesting

head_commit=$(git -C "$source_root" rev-parse HEAD)
short_commit=$(printf '%s' "$head_commit" | cut -c1-10)
version=$("$build_dir/bin/inkscape" --version 2>/dev/null || true)
printf '%s\n' "$version" | grep -Fq "$short_commit" || \
    fail "built executable is stale; expected commit $short_commit"

binary_architectures=$(uname -m)
binary_macos_minos=not-applicable
if [ "$(uname -s)" = "Darwin" ]; then
    python3 "$packaging_provenance" verify-inputs "$packaging_inputs" --runtime ||
        fail "packaging inputs changed during critical tests"
    for tool in lipo vtool; do
        command -v "$tool" >/dev/null 2>&1 || fail "required tool not found: $tool"
    done
    binary_architectures=$(lipo -archs "$build_dir/bin/inkscape")
    binary_macos_minos=$(vtool -show-build "$build_dir/bin/inkscape" |
        awk '$1 == "minos" {print $2; exit}')
    [ "$binary_macos_minos" = "$expected_deployment_target" ] ||
        fail "binary minimum macOS is '$binary_macos_minos'; expected $expected_deployment_target"

    runtime_log=$evidence_dir/runtime-libraries.log
    env DYLD_LIBRARY_PATH="$cairo_prefix/lib" DYLD_PRINT_LIBRARIES=1 \
        "$build_dir/bin/inkscape" --version >"$runtime_log" 2>&1 ||
        fail "could not inspect build runtime dependencies"
    grep -Fq "$libcdr_prefix/" "$runtime_log" ||
        fail "runtime did not load libcdr from the attested prefix"
    grep -Fq "$cairo_prefix/" "$runtime_log" ||
        fail "runtime did not load Cairo from the patched prefix"
    cairo_runtime_count=$(grep -E '/libcairo\.2\.dylib' "$runtime_log" | sort -u | wc -l | tr -d ' ')
    [ "$cairo_runtime_count" -eq 1 ] ||
        fail "runtime loaded $cairo_runtime_count distinct Cairo libraries"
    foreign_cairo=$(grep -E '/libcairo([.-]|$)' "$runtime_log" |
        grep -Fv "$cairo_prefix/" || true)
    [ -z "$foreign_cairo" ] || {
        printf '%s\n' "$foreign_cairo" >&2
        fail "runtime loaded Cairo components outside the attested prefix"
    }
fi

if [ "$mode" = "--quick" ]; then
    close_suites
    verify_run "before quick completion"
    echo "VACards $release_version quick regression gate passed for $short_commit (exact critical/native suites; no packaging authorization)."
    exit 0
fi

cmake --build "$build_dir" --parallel "$jobs" --target tests unit_tests || \
    fail "complete test suite did not build"
# `cmake --install` does not build missing targets. Build the default target as
# well so auxiliary executables such as inkview and every other install input
# are present before any test result can authorize an installation.
cmake --build "$build_dir" --parallel "$jobs" || \
    fail "complete installable application did not build"
verify_run "after full builds"
if [ "$(uname -s)" = "Darwin" ]; then
    python3 "$packaging_provenance" verify-inputs "$packaging_inputs" --runtime ||
        fail "full build changed tested inputs; preserve this run and configure a new tree"
fi
run_suite actionable
run_suite portfolio_short
run_suite portfolio_60s
close_suites
if [ "$(uname -s)" = "Darwin" ]; then
    python3 "$packaging_provenance" verify-inputs "$packaging_inputs" --runtime ||
        fail "packaging inputs changed during full suites"
fi
require_external_acceptance
# Retained legacy publication is unreachable without both independent owners.
# Structural closure alone must never unlock it or upgrade format-3 evidence.
critical_count=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["test_count"])' \
    "$evidence_dir/critical-tests.result.json")

verify_run "after actionable tests / before install"
cmake --install "$build_dir" --prefix "$install_prefix" || fail "installation failed"
verify_run "after install"
executable=$install_prefix/bin/inkscape
[ -x "$executable" ] || fail "installed executable is missing"
installed_version=$("$executable" --version 2>/dev/null || true)
printf '%s\n' "$installed_version" | grep -Fq "$short_commit" || \
    fail "installed executable is stale"
binary_sha256=$(shasum -a 256 "$executable" | awk '{print $1}')
dependency_manifest_sha256=$("$dependency_tool" --sha256)
libcdr_manifest_sha256=$(shasum -a 256 "$libcdr_manifest" | awk '{print $1}')
libcdr_library_sha256=$(manifest_value "$libcdr_manifest" library_sha256)
cmake_cache_sha256=$(shasum -a 256 "$cache" | awk '{print $1}')
cmake_features_sha256=$(shasum -a 256 "$resolved_features" | awk '{print $1}')
if [ "$(uname -s)" = "Darwin" ]; then
    python3 "$packaging_provenance" verify-inputs "$packaging_inputs" --runtime ||
        fail "packaging inputs changed during full tests/install"
    cp "$packaging_inputs" "$install_prefix/VACARDS-PACKAGING-INPUTS.json"
    cp "$packaging_inputs" "$evidence_dir/VACARDS-PACKAGING-INPUTS.json"
    cp "$cairo_prefix/VACARDS-CAIRO.txt" "$install_prefix/VACARDS-CAIRO.txt"
    cp -R "$tested_schemas" "$install_prefix/VACARDS-GTK-SCHEMAS"
    packaging_inputs_sha256=$(shasum -a 256 "$packaging_inputs" | awk '{print $1}')
fi
# Preserve the exact local reservation in the existing provenance hash chain.
# This does not authenticate test selection, the baseline, or the ledger itself.
gate_run_record=$build_dir/VACARDS-GATE-RUN.json
gate_run_sha256=$(shasum -a 256 "$gate_run_record" | awk '{print $1}')
cp "$gate_run_record" "$install_prefix/VACARDS-GATE-RUN.json"
cp "$gate_run_record" "$evidence_dir/VACARDS-GATE-RUN.json"
build_provenance=$install_prefix/VACARDS-BUILD-PROVENANCE.env
compiler=$(cache_value CMAKE_CXX_COMPILER)
compiler_version=$("$compiler" --version 2>/dev/null | head -n 1 || true)
pkgconfig_packages='gtk4 gtkmm-4.0 glib-2.0 glibmm-2.68 pango pangomm-2.48 harfbuzz cairo cairomm-1.16 gdk-pixbuf-2.0 librsvg-2.0 libcdr-0.1 librevenge-0.0 libwpg-0.3 libvisio-0.1 libspelling-1 gtksourceview-5 poppler poppler-glib lcms2 libpng libtiff-4 freetype2 fontconfig'
{
    printf 'format=1\n'
    printf 'gate_run_sha256=%s\n' "$gate_run_sha256"
    printf 'cmake_version=%s\n' "$(cmake --version | awk 'NR == 1 {print $3}')"
    printf 'cmake_generator=%s\n' "$(cache_value CMAKE_GENERATOR)"
    printf 'cmake_build_type=%s\n' "$(cache_value CMAKE_BUILD_TYPE)"
    printf 'compiler=%s\n' "$compiler"
    printf 'compiler_version=%s\n' "$compiler_version"
    printf 'libcdr_prefix=%s\n' "$libcdr_prefix"
    printf 'libcdr_version=%s\n' "$expected_libcdr_version"
    if [ "$(uname -s)" = "Darwin" ]; then
        printf 'packaging_inputs_sha256=%s\n' "$packaging_inputs_sha256"
        printf 'cairo_prefix=%s\n' "$cairo_prefix"
        printf 'cairo_version=%s\n' "$(cache_value VACARDS_RESOLVED_CAIRO_VERSION)"
    fi
    printf 'binary_architectures=%s\n' "$binary_architectures"
    printf 'macos_deployment_target=%s\n' "$binary_macos_minos"
    for package in $pkgconfig_packages; do
        package_version=$(pkg-config --modversion "$package" 2>/dev/null || true)
        [ -n "$package_version" ] || fail "required pkg-config package disappeared: $package"
        package_key=$(printf '%s' "$package" | tr '.+-' '___')
        printf 'pkgconfig_%s=%s\n' "$package_key" "$package_version"
    done
} > "$build_provenance"
build_provenance_sha256=$(shasum -a 256 "$build_provenance" | awk '{print $1}')
cp "$dependency_manifest" "$install_prefix/VACARDS-DEPENDENCIES.env"
cp "$libcdr_manifest" "$install_prefix/VACARDS-LIBCDR.env"
cp "$resolved_features" "$install_prefix/VACARDS-CMAKE-FEATURES.env"
attestation=$install_prefix/VACARDS-RELEASE-GATE.env
candidate=$install_prefix/VACARDS-RELEASE-GATE.pending.env
evidence_candidate=$evidence_dir/VACARDS-RELEASE-GATE.pending.env
created_utc=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

verify_run "before final attestation"
{
    echo "format=3"
    echo "scope=full"
    echo "base_version=$base_version"
    echo "build_number=$build_number"
    echo "release_version=$release_version"
    echo "source_commit=$head_commit"
    echo "source_baseline_commit=$source_baseline_commit"
    echo "binary_sha256=$binary_sha256"
    echo "binary_architectures=$binary_architectures"
    echo "macos_deployment_target=$binary_macos_minos"
    echo "dependency_manifest_sha256=$dependency_manifest_sha256"
    echo "libcdr_commit=$expected_libcdr_commit"
    echo "libcdr_version=$expected_libcdr_version"
    echo "libcdr_library_sha256=$libcdr_library_sha256"
    echo "libcdr_manifest_sha256=$libcdr_manifest_sha256"
    echo "build_provenance_sha256=$build_provenance_sha256"
    echo "cmake_cache_sha256=$cmake_cache_sha256"
    echo "cmake_features_sha256=$cmake_features_sha256"
    echo "submodules_sha256=$submodules_sha256"
    echo "critical_test_count=$critical_count"
    echo "created_utc=$created_utc"
} | python3 -c 'import sys
with open(sys.argv[1], "x", encoding="utf-8") as stream:
    stream.write(sys.stdin.read())
' "$candidate" || fail "could not exclusively write pending attestation; preserve this run"
# Stage the evidence copy without exposing either canonical filename. Each
# directory later uses its own same-filesystem link, even across volumes.
python3 -c 'import sys
with open(sys.argv[1], "rb") as source, open(sys.argv[2], "xb") as destination:
    destination.write(source.read())
' "$candidate" "$evidence_candidate" || fail "could not exclusively stage pending evidence; preserve this run"

"$script_dir/verify-release-attestation.sh" --candidate "$install_prefix" ||
    fail "pending attestation verification failed; preserved without packaging authorization"
verify_run "after candidate validation / before publication"
# Publish only validated bytes. link(2) atomically refuses every existing target
# (including directories/symlinks); no cleanup must succeed to withhold authority.
# Keep the pending name as evidence. It is never accepted by the bundler.
# Evidence is not an installable prefix. Publish it first; if the final install
# link fails, retain that validated evidence without install authorization.
python3 -c 'import os, pathlib, sys
candidate, final, evidence_candidate, evidence_final = map(pathlib.Path, sys.argv[1:])
if candidate.read_bytes() != evidence_candidate.read_bytes():
    raise ValueError("pending evidence differs from validated candidate")
for destination in (final, evidence_final):
    if os.path.lexists(destination):
        raise FileExistsError("publication destination already exists: " + str(destination))
os.link(evidence_candidate, evidence_final, follow_symlinks=False)
os.link(candidate, final, follow_symlinks=False)  # Last authorizing operation.
' "$candidate" "$attestation" "$evidence_candidate" "$evidence_dir/VACARDS-RELEASE-GATE.env" ||
    fail "could not exclusively publish attestation; validated pending records preserved"
echo "VACards full release gate passed for $short_commit."
echo "Release: $release_version (build $build_number)"
echo "Attestation: $attestation"
