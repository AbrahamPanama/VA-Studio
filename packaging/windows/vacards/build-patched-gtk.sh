#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a patched UCRT64 GTK 4.22.4 for VACards.
#
# Reproduces the MSYS2 mingw-w64-gtk4 4.22.4-1 recipe (MINGW-packages commit
# d07b8dabb92443fd1daed511ad7b406825964b0d) and adds
# packaging/windows/vacards/gtk-4.22.4-win32-cairo-buffer.patch.
#
# Hard rules:
#   * Never pacman-install, -Sy/-Sw, upgrade, or write into /ucrt64 or any other
#     system prefix. Missing build tools are extracted from pinned official
#     MSYS2 packages into an own prefix (--build-tools-prefix).
#   * The Vulkan runtime feature is preserved. There is no option to disable it;
#     a missing glslc or Vulkan header is a hard failure.
#   * Only build-time introspection (default disabled) and man-pages (default
#     enabled) may be turned off, each with an explicit flag and a recorded
#     deviation. Neither changes the DLL's exported/imported symbol set.
#   * Configure and compile require a root approval record whose patch sha256
#     matches the exact implementation patch being built. Absent or mismatched
#     approval is fatal and nothing is configured or compiled.
#   * Every run uses a unique, initially empty output root; failed runs keep
#     their logs and are never rewritten to look like the first attempt.
#   * Hash-pinned inputs only. Missing or mismatched input is a hard failure.
#   * Compile is limited to --jobs (default and maximum 2). Tests are not run
#     here; GUI/pixel qualification is separate.
#
# See doc/vacards/WINDOWS_CAIRO_BUFFER_BUILD.md for provenance and staging.
set -euo pipefail
export LC_ALL=C

fail() { echo "VACards Windows patched-GTK build failed: $*" >&2; exit 1; }

# --- pinned GTK inputs -------------------------------------------------------
gtk_version=4.22.4
gtk_tarball=gtk-${gtk_version}.tar.xz
gtk_sha=51bd9f60c7d23a665a556c7364c21fb2e4e282566b3e7e092455e8f910330893
p001=001-fix-font-rendering.patch
p001_sha=a2c6e3350bd9c1744da6b7714b25cbd419645b731435f7d295a9f99da3c1479f
p003=003-default-dcomp-off.patch
p003_sha=dda6bbab33b12ce50cb8706e7b9970f040dde4ba7cf2fe89c6f3842809fc0e87
pkgbuild=PKGBUILD
pkgbuild_sha=73e4e8eb077a27447a639015286b05774cfff26f87ca7e2e5ca21b79a912fd16
msys2_commit=d07b8dabb92443fd1daed511ad7b406825964b0d
gtk_source_url="https://download.gnome.org/sources/gtk/${gtk_version:0:4}/${gtk_tarball}"
msys2_raw_base="https://raw.githubusercontent.com/msys2/MINGW-packages/${msys2_commit}/mingw-w64-gtk4"

# --- pinned build-tool packages (official MSYS2 ucrt64, 2026-09-21) -----------
# Package sha256 values were read from the signed
# /var/lib/pacman/sync/ucrt64.db, then re-verified against the downloaded bytes.
# glslc.exe from shaderc links no shaderc/glslang DLL (statically linked); the
# glslang and spirv-tools packages are the declared shaderc dependencies and are
# kept so the pin mirrors the official dependency closure.
tools_mirror="https://mirror.msys2.org/mingw/ucrt64"
tools_pkgs=(
    "mingw-w64-ucrt-x86_64-shaderc|2026.3-1|0af3be0981f194806859a521c8ea944e084a69bb720d59aae2adedfd88c14adc"
    "mingw-w64-ucrt-x86_64-glslang|16.3.0-1|5a90c8ae45b42b500fa03669aa1a94d154dcd13e0bb254f86df004b903826c03"
    "mingw-w64-ucrt-x86_64-spirv-tools|3~1.4.357.0-1|ba11b4bad606f6ececc5bf5910f97077071624851c81a25fafc6b0ec88c58b84"
    "mingw-w64-ucrt-x86_64-vulkan-headers|1~1.4.357.0-1|96749e9c3c10bd7dd6624530006b4ed5c165c0f1bc37125522e046c63aed649c"
    "mingw-w64-ucrt-x86_64-vulkan-loader|1~1.4.357.0-1|4d34983aea34116582700b285279df21cd0ee8519025330e7ac0650bb02f138f"
    "mingw-w64-ucrt-x86_64-python-docutils|0.22.4-1|cceda7bb5b939b52ff6cdb1f4a003d47c07d0f4fa60b0f0918349bec457d4dd4"
    "mingw-w64-ucrt-x86_64-python-mako|1.4.1-1|42c42a1d4dbf5f863448188cf02748c34ad12d76ad1a5b7589d45fed6d17c3da"
    "mingw-w64-ucrt-x86_64-python-markdown|3.10.3-1|8445ce5e79eb123ddfbc65105cacd096e34d0d256708eb4f5455d48a51e6d895"
)
glslc_version=2026.3
vulkan_version=1.4.357
docutils_version=0.22.4

usage() {
    cat <<'EOF'
usage: build-patched-gtk.sh --output EMPTY_DIR [options]

Required for a build:
  --output DIR              unique, initially empty output root (outside source)

Inputs (defaults look next to the script / documented dep prefixes):
  --seed-dir DIR            directory with gtk-4.22.4.tar.xz, the two MSYS2
                            patches, and msys2-recipe/PKGBUILD or PKGBUILD
  --download                fetch missing pinned inputs from their official URLs
  --impl-patch FILE         implementation patch (default: sibling
                            gtk-4.22.4-win32-cairo-buffer.patch)
  --cairo-prefix DIR        pinned Cairo prefix (default: VACARDS_CAIRO_PREFIX or
                            /c/vacards/deps/cairo-parity-20260906-r2/install)

Build tools (never installed into /ucrt64):
  --build-tools-prefix DIR  own prefix for pinned MSYS2 tools (default:
                            VACARDS_GTK_TOOLS_PREFIX or <script dir>/gtk-build-tools)
  --provision-build-tools   download, hash-verify and extract the pinned MSYS2
                            packages into the tools prefix, then exit

Build control:
  --jobs N                  compile jobs, 1 or 2 (default 2)
  --configure-only          apply patches and run meson setup, do not compile
  --enable-introspection    build GIR (default off: Python 3.14 + MSYS2
                            g-object-introspection lacks distutils; recorded)
  --disable-man-pages       skip man pages (default: built with rst2man)
  --approval FILE           root approval JSON (default:
                            $source_root/.dsh-report/ROOT-BUILD-APPROVAL.json or
                            VACARDS_BUILD_APPROVAL); patch sha must match
  -h, --help

Vulkan is always enabled. Configure/compile never start without a matching root
approval record.
EOF
}

output_arg= seed_dir= impl_patch= cairo_prefix= jobs=2 configure_only=0 download=0
build_tools_prefix_arg= provision=0 enable_introspection=0 disable_man_pages=0
approval_arg=
while [[ $# -gt 0 ]]; do
    case $1 in
        --output) output_arg=${2:-}; shift 2 ;;
        --seed-dir) seed_dir=${2:-}; shift 2 ;;
        --impl-patch) impl_patch=${2:-}; shift 2 ;;
        --cairo-prefix) cairo_prefix=${2:-}; shift 2 ;;
        --build-tools-prefix) build_tools_prefix_arg=${2:-}; shift 2 ;;
        --provision-build-tools) provision=1; shift ;;
        --jobs) jobs=${2:-}; shift 2 ;;
        --configure-only) configure_only=1; shift ;;
        --enable-introspection) enable_introspection=1; shift ;;
        --disable-man-pages) disable_man_pages=1; shift ;;
        --approval) approval_arg=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; fail "unknown argument: $1" ;;
    esac
done

[[ $jobs == 1 || $jobs == 2 ]] || fail "--jobs must be 1 or 2 (project limit)"

# --- host / toolchain checks -------------------------------------------------
[[ ${MSYSTEM:-} == UCRT64 && $(uname -m) == x86_64 ]] || fail "requires an x86_64 MSYS2 UCRT64 shell"
case $(uname -s) in MINGW*_NT-*) ;; *) fail "requires a native Windows UCRT64 host" ;; esac
for tool in bash cygpath tar patch meson ninja gcc objdump awk find sort grep cmp tee sha256sum; do
    command -v "$tool" >/dev/null || fail "missing tool: $tool"
done

pkg_config=${PKG_CONFIG:-pkg-config}
command -v "$pkg_config" >/dev/null || fail "missing pkg-config: $pkg_config"
normalize_dir() { (cd -- "$(cygpath -u "$1")" && pwd -P); }
sha256() { sha256sum "$1" | awk '{print $1}'; }
fetch() { # url dest
    command -v curl >/dev/null || fail "download requires curl"
    curl --fail --location --proto '=https' --tlsv1.2 --output "$2" "$1"
}

script_dir=$(cd -- "$(dirname -- "$0")" && pwd -P)
source_root=$(cd -- "$script_dir/../../.." && pwd -P)

ucrt_prefix=$(normalize_dir /ucrt64)
gcc=$(command -v gcc)
[[ $(normalize_dir "$(dirname -- "$gcc")") == "$ucrt_prefix/bin" ]] || fail "compiler is outside /ucrt64/bin: $gcc"
[[ $("$gcc" -dumpmachine) == x86_64-w64-mingw32 ]] || fail "wrong compiler target: $gcc"
[[ -f $ucrt_prefix/bin/python.exe ]] || fail "missing /ucrt64/bin/python.exe (needed for rst2man)"

shell_pyver=$("$ucrt_prefix/bin/python.exe" -c 'import sys;print("python%d.%d"%sys.version_info[:2])') \
    || fail "cannot determine the UCRT64 Python version"

# Own build-tools prefix (never /ucrt64).
tools_prefix=${build_tools_prefix_arg:-${VACARDS_GTK_TOOLS_PREFIX:-}}
[[ -n $tools_prefix ]] || tools_prefix=$script_dir/gtk-build-tools
tools_prefix=$(cygpath -u "$tools_prefix" 2>/dev/null || printf '%s' "$tools_prefix")
[[ -n $tools_prefix && $tools_prefix == /* ]] || fail "use an absolute --build-tools-prefix"
case $tools_prefix in "$ucrt_prefix"|"$ucrt_prefix"/*|/usr|/usr/*|/bin|/bin/*|/lib|/lib/*|/etc|/etc/*) fail "refusing a system build-tools prefix: $tools_prefix" ;; esac
mkdir -p -- "$tools_prefix"
tools_prefix=$(normalize_dir "$tools_prefix") || fail "cannot resolve --build-tools-prefix"

# Feature decisions. Vulkan is always enabled; introspection is off by default
# (root-approved build-time-only deviation) and man-pages default on.
man_pages=true
[[ $disable_man_pages == 0 ]] || man_pages=false
introspection=disabled
[[ $enable_introspection == 0 ]] || introspection=enabled

# --- build-tools provisioning and verification -------------------------------
# Scoped environment only: never PATH/PKG_CONFIG_PATH/C_INCLUDE_PATH into a
# system prefix. CPATH/LIBRARY_PATH point the compiler at this prefix's own
# Vulkan headers and import library; the installed vulkan.pc supplies version
# and -lvulkan-1, and pkgconf may resolve it on a name collision.
apply_scoped_env() {
    export PATH="$tools_prefix/bin:$PATH"
    export CPATH="$tools_prefix/include${CPATH:+:$CPATH}"
    export LIBRARY_PATH="$tools_prefix/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
    export PYTHONPATH="$tools_prefix/lib/$shell_pyver/site-packages${PYTHONPATH:+:$PYTHONPATH}"
    export SETUPTOOLS_USE_DISTUTILS=local
}

provision_build_tools() {
    command -v curl >/dev/null || fail "--provision-build-tools requires curl"
    local pkgdir=$tools_prefix/pkgs
    mkdir -p -- "$tools_prefix/bin" "$pkgdir"
    local prov=$tools_prefix/TOOLS-PROVENANCE.txt
    {
        printf 'format=1\nplatform=windows-ucrt64\narchitecture=x86_64\n'
        printf 'mirror=%s\nprovisioned_utc=%s\n' "$tools_mirror" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } > "$prov"
    local spec name ver want file sha
    for spec in "${tools_pkgs[@]}"; do
        IFS='|' read -r name ver want <<< "$spec"
        file=$name-$ver-any.pkg.tar.zst
        if [[ ! -f $pkgdir/$file ]]; then
            fetch "$tools_mirror/$file" "$pkgdir/$file.part"
            mv -- "$pkgdir/$file.part" "$pkgdir/$file"
        fi
        sha=$(sha256 "$pkgdir/$file")
        [[ $sha == "$want" ]] || fail "tool package checksum mismatch for $file (got $sha)"
        tar --zstd -xf "$pkgdir/$file" -C "$tools_prefix" --strip-components=1 \
            --exclude=.BUILDINFO --exclude=.MTREE --exclude=.PKGINFO
        printf 'package=%s version=%s sha256=%s url=%s/%s\n' \
            "$name" "$ver" "$want" "$tools_mirror" "$file" >> "$prov"
    done

    # Relocate the official vulkan.pc to this prefix. Header and loader bytes are
    # the untouched official package bytes; only the embedded prefix changes.
    local vpc=$tools_prefix/lib/pkgconfig/vulkan.pc
    [[ -f $vpc ]] || fail "vulkan.pc missing after extraction"
    cat > "$vpc" <<EOF
prefix=$tools_prefix
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: Vulkan-Loader
Description: Vulkan Loader (official MSYS2 package, prefix relocated)
Version=$vulkan_version
Libs: -L\${libdir} -lvulkan-1
Cflags: -I\${includedir}
EOF

    # setuptools console-script .exe wrappers look for python.exe beside the
    # wrapper; provide a reference to the read-only system interpreter.
    rm -f -- "$tools_prefix/bin/python.exe"
    cp -- "$ucrt_prefix/bin/python.exe" "$tools_prefix/bin/python.exe"
    [[ -f $tools_prefix/bin/python.exe ]] || fail "python.exe shim missing"
    [[ -d $tools_prefix/lib/$shell_pyver/site-packages ]] || fail "docutils site-packages missing"

    apply_scoped_env
    ( cd -- "$tools_prefix" && \
      find bin include lib share -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum ) \
        > "$tools_prefix/TOOLS.sha256"
    check_build_tools
    printf 'glslc=%s\nvulkan_header=%s\nvulkan_pc=%s\nresolved_vulkan_pc=%s\nvulkan_probe=%s\ntools_inventory_sha256=%s\n' \
        "$tools_prefix/bin/glslc.exe" "$tools_prefix/include/vulkan/vulkan.h" \
        "$tools_prefix/lib/pkgconfig/vulkan.pc" "$resolved_vulkan_pcfile" "$vulkan_probe_result" \
        "$(sha256 "$tools_prefix/TOOLS.sha256")" >> "$prov"
}

check_build_tools() {
    local glslc=$tools_prefix/bin/glslc.exe out probe_dir
    vulkan_probe_result=not-run
    resolved_vulkan_pcfile=
    [[ -f $glslc ]] || fail "glslc missing in $tools_prefix/bin (run --provision-build-tools)"
    out=$("$glslc" --version 2>&1 | head -1) || fail "glslc failed to run"
    [[ $out == "$glslc_version" ]] || fail "glslc version '$out' != pinned $glslc_version"
    [[ -f $tools_prefix/include/vulkan/vulkan.h ]] || fail "vulkan/vulkan.h missing in $tools_prefix/include"
    [[ -f $tools_prefix/lib/libvulkan-1.dll.a ]] || fail "libvulkan-1.dll.a missing in $tools_prefix/lib"
    [[ -f $tools_prefix/lib/pkgconfig/vulkan.pc ]] || fail "vulkan.pc missing in $tools_prefix/lib/pkgconfig"
    out=$(PKG_CONFIG_PATH="$tools_prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
          "$pkg_config" --modversion vulkan 2>&1) || fail "pkg-config cannot resolve vulkan"
    [[ $out == "$vulkan_version" ]] || fail "vulkan pkg-config version '$out' != $vulkan_version"
    resolved_vulkan_pcfile=$(PKG_CONFIG_PATH="$tools_prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}" \
          "$pkg_config" --variable=pcfiledir vulkan 2>&1) \
        || fail "cannot query vulkan pcfiledir"
    # Real compile+link probe: the extracted Vulkan header is found through
    # CPATH and the extracted import library through LIBRARY_PATH.
    probe_dir=$tools_prefix/.vulkan-probe.$$
    mkdir -p -- "$probe_dir"
    cat > "$probe_dir/probe.c" <<'EOF'
#include <vulkan/vulkan.h>
int main(void) { return VK_API_VERSION_MAJOR(VK_HEADER_VERSION_COMPLETE) == 1 ? 0 : 1; }
EOF
    if ! "$gcc" $(PKG_CONFIG_PATH="$tools_prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}" "$pkg_config" --cflags vulkan) \
            "$probe_dir/probe.c" -o "$probe_dir/probe.exe" \
            $(PKG_CONFIG_PATH="$tools_prefix/lib/pkgconfig:${PKG_CONFIG_PATH:-}" "$pkg_config" --libs vulkan) \
            2>"$probe_dir/probe.err"; then
        out=$(tail -1 "$probe_dir/probe.err")
        rm -rf -- "$probe_dir"
        fail "Vulkan header/library probe failed to compile/link: $out"
    fi
    if ! "$probe_dir/probe.exe"; then
        rm -rf -- "$probe_dir"
        fail "Vulkan probe binary returned nonzero"
    fi
    rm -rf -- "$probe_dir"
    vulkan_probe_result=passed
    if [[ $man_pages == true ]]; then
        out=$(PYTHONPATH="$tools_prefix/lib/$shell_pyver/site-packages${PYTHONPATH:+:$PYTHONPATH}" \
              "$tools_prefix/bin/rst2man.exe" --version 2>&1 | head -1) || fail "rst2man failed to run (man-pages enabled)"
        [[ $out == *"Docutils $docutils_version"* ]] || fail "rst2man version '$out' != Docutils $docutils_version"
    fi
    if [[ $introspection == enabled ]]; then
        out=$("$tools_prefix/bin/g-ir-scanner.exe" --version 2>&1) \
            || fail "g-ir-scanner is not functional; refusing to enable introspection"
        [[ $out == *"1.86.0"* ]] || fail "unexpected g-ir-scanner output: $out"
    fi
}

if [[ $provision == 1 ]]; then
    provision_build_tools
    echo "Provisioned and verified pinned build tools in $tools_prefix"
    echo "Provenance: $tools_prefix/TOOLS-PROVENANCE.txt"
    exit 0
fi

# --- output root -------------------------------------------------------------
[[ -f $source_root/CMakeLists.txt ]] || fail "source_root does not resolve the VACards repository: $source_root"
[[ -n $output_arg ]] || { usage >&2; fail "missing --output"; }
output=$(cygpath -u "$output_arg")
[[ -n $output && $output == /* ]] || fail "use an absolute --output directory"
if [[ -e $output ]]; then
    [[ -d $output && ! -L $output ]] || fail "output is not a regular directory"
    [[ -z $(find "$output" -mindepth 1 -maxdepth 1 -print -quit) ]] || fail "output must be empty; use a new unique root per attempt"
fi
parent=$(normalize_dir "$(dirname -- "$output")") || fail "output parent must exist"
output=$parent/$(basename -- "$output")
case $output in "$ucrt_prefix"|"$ucrt_prefix"/*|"$tools_prefix"|"$tools_prefix"/*|/usr/*|/bin/*|/lib/*|/etc/*) fail "refusing a system or tools directory" ;; esac
mkdir -p -- "$output"
output=$(normalize_dir "$output")
prefix=$output/install
build_dir=$output/build
src_dir=$output/src
inputs=$output/inputs
mkdir -p -- "$inputs" "$src_dir"

trap 'rc=$?; printf "%s\n" "$rc" > "$output/exit-code.txt"; [[ $rc -eq 0 ]] || printf "failed\n" > "$output/FAILED"; exit $rc' EXIT

# --- pinned inputs -----------------------------------------------------------
[[ -n $impl_patch ]] || impl_patch=$script_dir/gtk-${gtk_version}-win32-cairo-buffer.patch
[[ -n $cairo_prefix ]] || cairo_prefix=${VACARDS_CAIRO_PREFIX:-/c/vacards/deps/cairo-parity-20260906-r2/install}
seed_dir=${seed_dir:-$script_dir/gtk-pinned-inputs}
seed_dir=$(cygpath -u "$seed_dir" 2>/dev/null || printf '%s' "$seed_dir")
cairo_prefix=$(normalize_dir "$cairo_prefix") || fail "Cairo prefix not found: $cairo_prefix"
[[ -f $impl_patch ]] || fail "implementation patch not found: $impl_patch"
impl_dir=$(normalize_dir "$(dirname -- "$impl_patch")") || fail "implementation patch directory not found"
impl_patch=$impl_dir/$(basename -- "$impl_patch")

seed_file() { # name -> path, preferring the seed, else the output cache
    local name=$1
    if [[ -f $seed_dir/$name ]]; then printf '%s' "$seed_dir/$name"
    elif [[ -f $seed_dir/downloads/$name ]]; then printf '%s' "$seed_dir/downloads/$name"
    elif [[ -f $seed_dir/msys2-recipe/$name ]]; then printf '%s' "$seed_dir/msys2-recipe/$name"
    elif [[ -f $inputs/$name ]]; then printf '%s' "$inputs/$name"
    else return 1; fi
}

acquire() { # name sha url
    local name=$1 want=$2 url=$3 src
    if src=$(seed_file "$name"); then
        cp -- "$src" "$inputs/$name"
    elif [[ $download == 1 ]]; then
        fetch "$url" "$inputs/$name"
    else
        fail "missing pinned input $name (pass --seed-dir or --download)"
    fi
    [[ $(sha256 "$inputs/$name") == "$want" ]] || fail "checksum mismatch for $name"
}

acquire "$gtk_tarball" "$gtk_sha" "$gtk_source_url"
acquire "$p001" "$p001_sha" "$msys2_raw_base/$p001"
acquire "$p003" "$p003_sha" "$msys2_raw_base/$p003"
acquire "$pkgbuild" "$pkgbuild_sha" "$msys2_raw_base/$pkgbuild"
cp -- "$impl_patch" "$inputs/gtk-${gtk_version}-win32-cairo-buffer.patch"
impl_sha=$(sha256 "$inputs/gtk-${gtk_version}-win32-cairo-buffer.patch")
patch_sha=$impl_sha

# --- root build approval gate (before configure/compile) ---------------------
approval=${approval_arg:-${VACARDS_BUILD_APPROVAL:-$source_root/.dsh-report/ROOT-BUILD-APPROVAL.json}}
[[ -n $approval ]] || fail "no approval path"
[[ -f $approval && ! -L $approval ]] || fail "root build approval missing: $approval (root must review the current patch and record its sha256)"
approval_sha=$(sha256 "$approval")
approved_patch=$(sed -nE 's/.*"(patch_sha256|impl_patch_sha256|gtk_patch_sha256|candidate_gtk_patch_sha256|patch_sha)"[[:space:]]*:[[:space:]]*"([0-9a-f]{64})".*/\2/p' "$approval" | head -1)
[[ -n $approved_patch ]] || fail "approval $approval has no patch sha256 field"
[[ $approved_patch == "$patch_sha" ]] || fail "approval patch sha $approved_patch != current patch $patch_sha; rebuild after root re-approval"
printf 'approval=%s\napproval_sha256=%s\napproved_patch_sha256=%s\n' "$approval" "$approval_sha" "$approved_patch" > "$inputs/approval.txt"

# --- extract and patch -------------------------------------------------------
tar -xf "$inputs/$gtk_tarball" -C "$src_dir"
source_dir=$src_dir/gtk-${gtk_version}
[[ -f $source_dir/meson.build ]] || fail "unexpected tarball layout"
[[ -f $source_dir/gtk/theme/Default/Default-light.css ]] || fail "tarball lacks pre-generated theme CSS; sassc is unavailable and required"

apply_patch() { # file label
    local file=$1 label=$2
    if ! patch --batch --forward --fuzz=2 -d "$source_dir" -p1 -i "$inputs/$file" 2>&1 | tee -a "$output/patch.log"; then
        fail "patch failed: $label"
    fi
    printf 'applied %s sha256=%s\n' "$label" "$(sha256 "$inputs/$file")" >> "$output/patch.log"
}

: > "$output/patch.log"
apply_patch "$p001" "MSYS2 001-fix-font-rendering"
apply_patch "$p003" "MSYS2 003-default-dcomp-off"
apply_patch "gtk-${gtk_version}-win32-cairo-buffer.patch" "VACards win32 cairo buffer"

grep -q 'GTK_FONT_RENDERING_MANUAL' "$source_dir/gtk/gtksettings.c" || fail "001 did not apply"
grep -q 'GDK_WIN32_FORCE_DCOMP' "$source_dir/gdk/win32/gdkdisplay-win32.c" || fail "003 did not apply"
grep -q 'gdi_buffer' "$source_dir/gdk/win32/gdkcairocontext-win32.c" || fail "implementation patch did not apply"

# --- scoped tool environment and tool verification ---------------------------
apply_scoped_env
check_build_tools
export PKG_CONFIG_PATH="$tools_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

# --- effective flags ---------------------------------------------------------
# MSYS2 installs makepkg_mingw.conf with pacman in the MSYS root /etc, not
# under the UCRT64 prefix; accept either, or an explicit override.
makepkg_conf=${VACARDS_MAKEPKG_MINGW_CONF:-}
if [[ -z $makepkg_conf ]]; then
    for candidate in "$ucrt_prefix/etc/makepkg_mingw.conf" "$ucrt_prefix/../etc/makepkg_mingw.conf"; do
        if [[ -f $candidate ]]; then makepkg_conf=$candidate; break; fi
    done
fi
[[ -n $makepkg_conf && -f $makepkg_conf ]] || fail "missing makepkg-mingw flags (looked in $ucrt_prefix/etc and $ucrt_prefix/../etc)"
# Read the installed recipe's linker flags, never an inherited environment guess.
# Current MSYS2 defines LDFLAGS="" for UCRT64: require it defined, allow empty.
makepkg_ldflags=$(bash -c 'set -e; unset LDFLAGS; source "$1"; printf "%s" "${LDFLAGS?missing LDFLAGS}"' _ "$makepkg_conf") \
    || fail "cannot read LDFLAGS from $makepkg_conf"
export CFLAGS='-O2 -march=nocona -msahf -mtune=generic -Wp,-D_FORTIFY_SOURCE=2 -fstack-protector-strong'
export CXXFLAGS=$CFLAGS
export LDFLAGS="$makepkg_ldflags -Wl,--no-insert-timestamp"
feature_args=(-Dman-pages="$man_pages" -Dvulkan=enabled -Dintrospection="$introspection")

# --- pinned Cairo: full identity gate via the existing verifier --------------
cairo_pc_dir=$cairo_prefix/lib/pkgconfig
[[ -f $cairo_pc_dir/cairo.pc ]] || fail "pinned Cairo pkg-config missing under $cairo_prefix"
export PKG_CONFIG_PATH="$cairo_pc_dir:$ucrt_prefix/lib/pkgconfig:$PKG_CONFIG_PATH"
cairo_version=$("$pkg_config" --modversion cairo)
[[ $cairo_version == 1.18.4 ]] || fail "pinned Cairo must be 1.18.4, got $cairo_version"
cairo_pc_filedir=$("$pkg_config" --variable=pcfiledir cairo)
cairo_verifier=${VACARDS_CAIRO_VERIFIER:-$source_root/packaging/windows/vacards/verify-vacards-cairo-prefix.sh}
[[ -f $cairo_verifier ]] || fail "Cairo verification gate not found: $cairo_verifier (set VACARDS_CAIRO_VERIFIER)"
if ! bash "$cairo_verifier" "$cairo_prefix" 2>&1 | tee "$output/cairo-verify.log"; then
    fail "pinned Cairo prefix failed verify-vacards-cairo-prefix.sh"
fi
cairo_marker_sha=$(sha256 "$cairo_prefix/VACARDS-CAIRO.env")
cairo_inventory_sha=$(sha256 "$cairo_prefix/VACARDS-CAIRO.sha256")

# --- record inputs and toolchain --------------------------------------------
inv=$inputs/inputs.sha256
(
    cd -- "$inputs"
    for f in "$gtk_tarball" "$p001" "$p003" "$pkgbuild" gtk-${gtk_version}-win32-cairo-buffer.patch; do
        printf '%s  %s\n' "$(sha256 "$f")" "$f"
    done
) > "$inv"

{
    printf 'format=2\nplatform=windows-ucrt64\narchitecture=x86_64\n'
    printf 'msystem=%s\n' "$MSYSTEM"
    printf 'gtk_version=%s\n' "$gtk_version"
    printf 'gtk_tarball_sha256=%s\n' "$gtk_sha"
    printf 'msys2_commit=%s\nmsys2_pkgbuild_sha256=%s\n' "$msys2_commit" "$pkgbuild_sha"
    printf 'p001_sha256=%s\np003_sha256=%s\n' "$p001_sha" "$p003_sha"
    printf 'impl_patch_sha256=%s\n' "$impl_sha"
    printf 'approval=%s\napproval_sha256=%s\n' "$approval" "$approval_sha"
    printf 'build_tools_prefix=%s\ntools_provenance_sha256=%s\ntools_inventory_sha256=%s\n' \
        "$tools_prefix" "$(sha256 "$tools_prefix/TOOLS-PROVENANCE.txt")" "$(sha256 "$tools_prefix/TOOLS.sha256")"
    printf 'glslc_version=%s\nvulkan_pkgconfig_version=%s\n' "$glslc_version" "$vulkan_version"
    printf 'resolved_vulkan_pcfile=%s\nvulkan_probe=%s\nvulkan_include=%s\nvulkan_import_lib=%s\n' \
        "$resolved_vulkan_pcfile" "$vulkan_probe_result" "$tools_prefix/include" "$tools_prefix/lib"
    printf 'pinned_cairo_prefix=%s\npinned_cairo_version=%s\npinned_cairo_pcfiledir=%s\n' \
        "$cairo_prefix" "$cairo_version" "$cairo_pc_filedir"
    printf 'cairo_marker_sha256=%s\ncairo_inventory_sha256=%s\n' "$cairo_marker_sha" "$cairo_inventory_sha"
    printf 'compiler=%s\ncompiler_target=%s\n' "$gcc" "$("$gcc" -dumpmachine)"
    printf 'makepkg_mingw_conf=%s\nmakepkg_mingw_conf_sha256=%s\n' "$makepkg_conf" "$(sha256 "$makepkg_conf")"
    printf 'CFLAGS=%s\nCXXFLAGS=%s\nLDFLAGS=%s\nmeson_prefix=C:/install\n' "$CFLAGS" "$CXXFLAGS" "$LDFLAGS"
    "$gcc" --version | head -1
    "$pkg_config" --modversion glib-2.0 gio-2.0 gdk-pixbuf-2.0 pango graphene-gobject-1.0 json-glib-1.0 epoxy gstreamer-1.0 2>/dev/null | paste -sd' ' - | sed 's/^/deps_versions=/' || true
    printf 'configure_only=%s\nintrospection=%s\nman_pages=%s\nintrospection_deviation=%s\n' \
        "$configure_only" "$introspection" "$man_pages" \
        "$([[ $introspection == disabled ]] && echo 'build-time-only; Python3.14 distutils unavailable for g-ir-scanner' || echo none)"
    printf 'meson_flags='
    printf '%q ' "${feature_args[@]}"
    printf '\n'
} > "$output/toolchain.txt"

# --- configure ---------------------------------------------------------------
if ! meson setup "$build_dir" "$source_dir" \
    --prefix=C:/install \
    --wrap-mode=nodownload \
    --auto-features=enabled \
    --buildtype=release \
    -Dbuild-examples=false \
    -Dbuild-tests=false \
    -Dbuild-testsuite=false \
    -Dmacos-backend=false \
    -Dmedia-gstreamer=enabled \
    -Dx11-backend=false \
    -Dwayland-backend=false \
    -Dwin32-backend=true \
    "${feature_args[@]}" \
    2>&1 | tee "$output/configure.log"; then
    fail "meson setup failed"
fi

if [[ $configure_only == 1 ]]; then
    echo "configure-only complete; compile was intentionally not started" | tee "$output/CONFIGURE-ONLY.txt"
    exit 0
fi

# --- compile (<=2 jobs) and install -----------------------------------------
if ! meson compile -C "$build_dir" -j "$jobs" 2>&1 | tee "$output/build.log"; then
    fail "meson compile failed"
fi
if ! meson install -C "$build_dir" --no-rebuild --destdir "$output" 2>&1 | tee "$output/install.log"; then
    fail "meson install failed"
fi

# --- output identities -------------------------------------------------------
dll=$prefix/bin/libgtk-4-1.dll
[[ -f $dll ]] || fail "installed DLL missing: $dll"
mkdir -p -- "$output/abi"
# Walk the GTK import closure against the staging source. System DLLs are not
# staged; every resolved UCRT64 DLL is recorded once by case-insensitive name.
pending=("$dll")
declare -A seen=()
seen[libgtk-4-1.dll]=1
: > "$output/abi/gtk-ucrt64-dlls.sha256"
while ((${#pending[@]})); do
    current=${pending[0]}
    pending=("${pending[@]:1}")
    headers=$(objdump -p "$current") || fail "cannot read imports: $current"
    imports=$(printf '%s\n' "$headers" | awk '/DLL Name:/{print $3}')
    [[ -n $imports ]] || fail "empty import table: $current"
    while IFS= read -r name; do
        lower=${name,,}
        [[ -z ${seen[$lower]+yes} ]] || continue
        seen[$lower]=1
        case $lower in api-ms-win-*|ext-ms-win-*) continue ;; esac
        if [[ $lower == libcairo-2.dll ]]; then
            [[ -f $cairo_prefix/bin/$name ]] || fail "pinned Cairo import missing: $name"
            pending+=("$cairo_prefix/bin/$name")
            continue
        fi
        dep=$(find "$ucrt_prefix/bin" -maxdepth 1 -iname "$name" -print -quit)
        if [[ -n $dep ]]; then
            printf '%s  %s\n' "$(sha256 "$dep")" "$lower" >> "$output/abi/gtk-ucrt64-dlls.sha256"
            pending+=("$dep")
        else
            [[ -f $(cygpath -u "$SYSTEMROOT")/System32/$name ]] || fail "unresolved GTK import: $name"
        fi
    done <<< "$imports"
done
LC_ALL=C sort -o "$output/abi/gtk-ucrt64-dlls.sha256" "$output/abi/gtk-ucrt64-dlls.sha256"
[[ -s $output/abi/gtk-ucrt64-dlls.sha256 ]] || fail "empty GTK UCRT64 DLL closure"
objdump -p "$dll" | awk '/\[Ordinal\/Name Pointer\] Table/{f=1;next} f&&/^$/{f=0} f&&/^[[:space:]]*\[[[:space:]]*[0-9]+\]/{print $NF}' \
    | LC_ALL=C sort -u > "$output/abi/libgtk-4-1.exports.txt"
(
    cd -- "$prefix"
    find bin lib -maxdepth 2 -type f \( -name 'libgtk-4-1.dll' -o -name 'libgtk-4.dll.a' \
        -o -name 'gtk4.pc' -o -name 'libgdk-4*.dll' -o -name 'libgsk-4*.dll' \) -print \
        | LC_ALL=C sort | while IFS= read -r f; do printf '%s  %s\n' "$(sha256 "$f")" "$f"; done
    printf '%s  abi/libgtk-4-1.exports.txt\n' "$(sha256 "$output/abi/libgtk-4-1.exports.txt")"
) > "$output/VACARDS-GTK.sha256"
marker=$output/VACARDS-GTK.env
{
    sed -n '1,200p' "$inv"
    printf 'prefix_posix=%s\nprefix_windows=%s\n' "$prefix" "$(cygpath -w "$prefix")"
    printf 'impl_patch_sha256=%s\napproval_sha256=%s\n' "$impl_sha" "$approval_sha"
    printf 'libgtk_4_1_dll_sha256=%s\n' "$(sha256 "$dll")"
    printf 'exports_sha256=%s\nexports_count=%s\n' \
        "$(sha256 "$output/abi/libgtk-4-1.exports.txt")" "$(wc -l < "$output/abi/libgtk-4-1.exports.txt")"
    printf 'inventory_sha256=%s\n' "$(sha256 "$output/VACARDS-GTK.sha256")"
    printf 'ucrt64_dll_manifest_sha256=%s\n' "$(sha256 "$output/abi/gtk-ucrt64-dlls.sha256")"
    printf 'configure_only=0\ncompile_jobs=%s\n' "$jobs"
} > "$marker"

# The tarball is adjacent to the run root so it cannot contain itself. The
# archive hash is appended afterwards; the archived toolchain is pre-archive.
archive=$output.tar.gz
[[ ! -e $archive ]] || fail "archive already exists: $archive"
printf '0\n' > "$output/exit-code.txt"
tar -czf "$archive" -C "$parent" "$(basename -- "$output")" || fail "run archive failed"
printf 'run_archive=%s\nrun_archive_sha256=%s\n' "$archive" "$(sha256 "$archive")" >> "$output/toolchain.txt"

echo "Patched UCRT64 GTK ${gtk_version} installed in $prefix"
echo "DLL sha256: $(sha256 "$dll")"
echo "Compare exports with the beta5 baseline and stage into a copied testapp; see WINDOWS_CAIRO_BUFFER_BUILD.md."
