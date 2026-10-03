// SPDX-License-Identifier: Apache-2.0

#include <capypdf.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

void require(bool value, const char *message) {
    if(!value) {
        throw std::runtime_error(message);
    }
}

std::string utf8(const fs::path &path) {
    const auto bytes = path.u8string();
    return {bytes.begin(), bytes.end()};
}

std::string read_pdf(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.is_open(), "Expected PDF filename was not created");
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    require(!stream.bad(), "Could not read PDF output");
    require(bytes.starts_with("%PDF-") && bytes.find("%%EOF") != std::string::npos,
            "Output lacks PDF header/trailer");
    return bytes;
}

void write_pdf(const fs::path &path, const char *title) {
    capypdf::DocumentProperties properties;
    properties.set_title(title);
    const auto filename = utf8(path);
    capypdf::Generator generator(filename.c_str(), properties);
    auto page = generator.new_page_context();
    page.cmd_rg(0.1, 0.4, 0.8);
    page.cmd_re(10, 10, 100, 60);
    page.cmd_f();
    generator.add_page(page);
    generator.write();
}

#ifdef _WIN32
struct LockedFile {
    HANDLE handle;
    explicit LockedFile(const fs::path &path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr)) {
        require(handle != INVALID_HANDLE_VALUE, "Could not lock existing PDF for regression");
    }
    ~LockedFile() { CloseHandle(handle); }
    LockedFile(const LockedFile &) = delete;
    LockedFile &operator=(const LockedFile &) = delete;
};
#endif

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc <= 2, "Usage: filepathtest [existing-output-parent]");
        const auto parent = argc == 2 ? fs::u8path(argv[1]) : fs::current_path();
        require(fs::is_directory(parent), "Output parent must already exist");
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto root = parent / ("capypdf-filepathtest-" + std::to_string(stamp));
        require(fs::create_directory(root), "Refusing to reuse a test directory");
        const auto directory = root / fs::path(u8"Prueba \u00f1 \u6d4b\u8bd5");
        require(fs::create_directory(directory), "Could not create Unicode fixture directory");
        const auto output = directory / fs::path(u8"salida \u00e1 \u6d4b\u8bd5.pdf");

        write_pdf(output, "First Unicode PDF");
        const auto first = read_pdf(output);
        std::cout << "PASS Unicode directory and filename\n";

        write_pdf(output, "Replacement Unicode PDF");
        const auto replacement = read_pdf(output);
        require(first != replacement, "Existing PDF was not replaced with new document bytes");
        std::cout << "PASS replace existing Unicode PDF\n";

#ifdef _WIN32
        {
            LockedFile lock(output);
            bool failed = false;
            try {
                write_pdf(output, "Must not replace locked destination");
            } catch(const capypdf::PdfException &) {
                failed = true;
            }
            require(failed, "Locked destination must report a PDF write error");
            require(read_pdf(output) == replacement, "Failed replacement changed existing PDF bytes");
        }
        std::cout << "PASS locked destination reports failure and preserves bytes\n";
        write_pdf(output, "Successful retry after releasing our lock");
        require(read_pdf(output) != replacement, "Export after releasing lock did not replace PDF");
        std::cout << "PASS export after releasing test-owned lock\n";
#else
        std::cout << "SKIP Windows sharing-lock test on this platform\n";
#endif
        std::cout << "Evidence retained at " << utf8(root) << '\n';
        return 0;
    } catch(const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
