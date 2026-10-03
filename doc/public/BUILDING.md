# Building VA Studio

VA Studio builds with CMake and Ninja like Inkscape, plus a Rust static library
(the nesting engine) and four dependencies that are patched or forked and built
first: Cairo, libcdr, librevenge and GTK. This repository is self-contained:
there are no submodules to fetch, the libcdr fork is in
`third_party/libcdr-vacards`, and the Rust crates are vendored. No account,
token or private repository is needed.

Run every build in its own new output directory, outside the source tree. The
dependency scripts refuse to overwrite an existing output.

## Pinned inputs

The single source of truth is `VACARDS-DEPENDENCIES.env`; the scripts check it.

| Input | Version | Pin |
| --- | --- | --- |
| Cairo | 1.18.4 + upstream fix `1d3347a6904f` | `packaging/macos/vacards/cairo-1.18.4-clip-all.patch` (both platforms) |
| libcdr | VA Studio fork, pkg-config version 0.1.10 | `third_party/libcdr-vacards`, commit `297ad1ad0b8e`, files listed in `third_party/libcdr-vacards.sha256` |
| librevenge | 0.0.6 + VA Studio patch | `packaging/dependencies/librevenge-0.0.6/` |
| GTK | 4.22.4 + VA Studio patches | macOS: `packaging/macos/vacards/gtk-4.22.4-*.patch`; Windows: `packaging/windows/vacards/gtk-4.22.4-win32-cairo-buffer.patch` on the MSYS2 recipe |
| Rust (nesting engine) | 1.88.0 | `src/3rdparty/vacards-nesting-rs/rust-toolchain.toml` |
| jagua-rs | 0.8.0, commit `9a19409bd38f` | vendored in `src/3rdparty/vacards-nesting-rs/vendor/` |
| Sparrow helper | commit `57c45cd295f5` | prebuilt executables in `src/3rdparty/sparrow/bin/`, SHA-256 pinned |
| macOS deployment target | 26.0 | |

### Upstream archives

The scripts verify these archives by SHA-256 before they use them.

| Archive | Official location | SHA-256 |
| --- | --- | --- |
| `cairo-1.18.4.tar.xz` | https://cairographics.org/releases/cairo-1.18.4.tar.xz | `445ed8208a6e4823de1226a74ca319d3600e83f6369f99b14265006599c32ccb` |
| `gtk-4.22.4.tar.xz` | https://download.gnome.org/sources/gtk/4.22/gtk-4.22.4.tar.xz | `51bd9f60c7d23a665a556c7364c21fb2e4e282566b3e7e092455e8f910330893` |
| `librevenge-0.0.6.tar.xz` | the libwpd project's librevenge releases, https://sourceforge.net/projects/libwpd/files/librevenge/ | `19eacf5ce55d7fe6a990a45142589cdf7da0c7b68701797f133482cb44f189fa` |
| MSYS2 `mingw-w64-gtk4` recipe (Windows) | https://github.com/msys2/MINGW-packages at commit `d07b8dabb92443fd1daed511ad7b406825964b0d`, files `PKGBUILD`, `001-fix-font-rendering.patch`, `003-default-dcomp-off.patch` | `73e4e8eb077a27447a639015286b05774cfff26f87ca7e2e5ca21b79a912fd16`, `a2c6e3350bd9c1744da6b7714b25cbd419645b731435f7d295a9f99da3c1479f`, `dda6bbab33b12ce50cb8706e7b9970f040dde4ba7cf2fe89c6f3842809fc0e87` |

The librevenge archive is not downloaded automatically: download it yourself
and pass its path; the SHA-256 above is what the scripts accept. The source
archive of each release (see `share/doc/SOURCE.md`) contains all of them.

### External inputs

- The RGB ICC profile embedded in TIFF exports (`tiff_rgb_profile_filename`,
  checked against `tiff_rgb_profile_sha256` in `VACARDS-DEPENDENCIES.env`).
- On Windows, the en_US and es hyphenation dictionaries with their license files.

They are not part of this repository; packaging scripts take them as parameters.

## macOS (Apple silicon)

1. Install Xcode Command Line Tools, [rustup](https://rustup.rs) and Homebrew,
   then the build dependencies:

   ```sh
   brew install autoconf automake bdw-gc boost cairomm cmake cppunit \
       double-conversion gettext googletest gsl gtk4 gtkmm4 gtksourceview5 \
       icu4c lcms2 libarchive libomp libtool libsigc++ libspelling libvisio \
       libwpg libxml2 libxslt meson ninja pango pkgconf poppler potrace
   ```

2. Build the patched and forked dependencies (each takes a new directory):

   ```sh
   packaging/macos/vacards/build-patched-cairo.sh ../deps/cairo
   packaging/macos/vacards/build-patched-librevenge.sh ../deps/librevenge ~/Downloads/librevenge-0.0.6.tar.xz
   packaging/macos/vacards/build-vacards-libcdr.sh ../deps/libcdr
   packaging/macos/vacards/build-patched-gtk.sh ~/Downloads/gtk-4.22.4.tar.xz "$PWD/../deps/gtk" "$PWD/../deps/cairo/install"
   ```

   `build-vacards-libcdr.sh` checks `third_party/libcdr-vacards` against its
   SHA-256 manifest, builds it, runs its tests and writes a provenance record.

3. Configure and build:

   ```sh
   export PKG_CONFIG_PATH="$PWD/../deps/gtk/install/lib/pkgconfig:$PWD/../deps/librevenge/install/lib/pkgconfig:$PWD/../deps/libcdr/install/lib/pkgconfig:$PWD/../deps/cairo/install/lib/pkgconfig"
   cmake -S . -B ../build -G Ninja \
       -DCMAKE_BUILD_TYPE=RelWithDebInfo \
       -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 \
       -DBUILD_TESTING=ON -DTESTS_WITH_ASAN=OFF \
       -DWITH_IMAGE_MAGICK=OFF -DWITH_GRAPHICS_MAGICK=OFF -DWITH_GNU_READLINE=OFF
   cmake --build ../build --parallel 2
   ```

   Configuration fails on purpose when it finds stock libcdr, unpatched Cairo
   or stock librevenge, or when a required feature in
   `VACARDS-DEPENDENCIES.env` (`required_cmake_features`) resolves off.
   For a release package, configure a separate build with
   `-DBUILD_TESTING=OFF`: test hooks are compiled in only when testing is on.

4. Check the libraries the binary loads:

   ```sh
   otool -L ../build/bin/inkscape | grep -E 'libcdr|cairo'
   packaging/macos/vacards/verify-patched-cairo-runtime.sh path/to/VA\ Studio.app
   ```

5. Package: `packaging/macos/vacards/create-internal-test-dmg.py --build ../build
   --output NEW_DIR --icc PROFILE.icc --gtk-prefix ../deps/gtk/install
   --gtk-sha256 SHA256` builds the application bundle and disk image used for
   the current releases, and checks the bundled libraries.

## Windows (x64, MSYS2 UCRT64)

1. Install [MSYS2](https://www.msys2.org) and open the **MSYS2 UCRT64** shell.
   Install the dependencies and the extra packages VA Studio needs:

   ```sh
   bash buildtools/msys2installdeps.sh
   pacman -S --needed mingw-w64-ucrt-x86_64-webview2-loader mingw-w64-ucrt-x86_64-nsis
   ```

   Install the Rust 1.88.0 toolchain for the `x86_64-pc-windows-gnu` target with
   [rustup](https://rustup.rs).

2. Build the dependencies (each takes a new, empty directory):

   ```sh
   packaging/windows/vacards/build-patched-cairo.sh /c/vas/deps/cairo
   packaging/windows/vacards/build-patched-librevenge.sh /c/vas/deps/librevenge --archive librevenge-0.0.6.tar.xz
   VACARDS_LIBREVENGE_PREFIX=/c/vas/deps/librevenge/install \
       packaging/windows/vacards/build-vacards-libcdr.sh /c/vas/deps/libcdr
   packaging/windows/vacards/build-patched-gtk.sh --provision-build-tools \
       --build-tools-prefix /c/vas/deps/gtk-tools
   packaging/windows/vacards/build-patched-gtk.sh --output /c/vas/deps/gtk \
       --download --build-tools-prefix /c/vas/deps/gtk-tools \
       --cairo-prefix /c/vas/deps/cairo/install --approval approval.json
   ```

   `build-patched-gtk.sh` builds only a patch you have reviewed: after reading
   `packaging/windows/vacards/gtk-4.22.4-win32-cairo-buffer.patch`, write its
   SHA-256 into `approval.json` as `{"patch_sha256": "<sha256>"}`. Its build
   tools are pinned MSYS2 packages extracted into their own prefix; nothing is
   installed into `/ucrt64`.

3. Configure and build:

   ```sh
   VACARDS_PATCHED_LIBREVENGE_PREFIX=/c/vas/deps/librevenge/install \
       packaging/windows/build-dev.sh /c/vas/build /c/vas/stage \
       /c/vas/deps/libcdr/install /c/vas/deps/cairo/install build
   ```

   `build-dev.sh` configures with testing enabled; for a release package use a
   separate build configured with `-DBUILD_TESTING=OFF`.

4. Package: `create-internal-test-installer.py stage` collects the application,
   the patched libraries, the GTK build (`--gtk-run`), the ICC profile and
   hyphenation dictionaries, and the MSYS2 runtime they need;
   `create-internal-test-installer.py pack --scope user|machine` builds the NSIS
   installer (`python packaging/windows/vacards/create-internal-test-installer.py --help`).

The Sparrow helper is prebuilt; `packaging/windows/vacards/build-sparrow.sh`
rebuilds the Windows executable from the pinned commit and lockfile with Rust
1.90.0 for comparison.

## Tests

Tests use CTest. Run them serially:

```sh
cmake --build ../build --parallel 2 --target build-vacards-critical
ctest --test-dir ../build --output-on-failure --parallel 1 --label-regex vacards-critical
```

The nesting engine also builds and tests on its own:

```sh
cmake -S src/3rdparty/vacards-nesting-rs -B ../nesting-build -G Ninja \
    -DBUILD_TESTING=ON -DVACARDS_NESTING_BUILD_PHASE0_TESTS=ON -DVACARDS_NESTING_BUILD_PHASE2_TESTS=ON
cmake --build ../nesting-build --parallel 2
ctest --test-dir ../nesting-build --output-on-failure --parallel 1
```

Some tests need a desktop session (GTK windows), and a few use sample files
that are not distributed; they report themselves as skipped or failed. Report
skips and failures as such rather than as passes.

## Inkscape build documentation

The upstream Inkscape build guides in [doc/building/](../building/) describe the
general toolchain. They do not cover VA Studio's patched dependencies.
