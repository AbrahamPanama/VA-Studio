// SPDX-License-Identifier: GPL-2.0-or-later
//
// Receiver for the external-process clipboard paste protocol (milestone A,
// E-series). One fresh process per case, launched by
// testfiles/clipboard/run-external-paste.py. The process never writes the
// clipboard: it asserts that the live clipboard belongs to a DIFFERENT process
// before it drives the real Paste action and the real ClipboardManager.
//
// Required environment (set by the driver):
//   VACARDS_CLIP_SESSION  driver session dir (lease.json, pub/ready.json)
//   VACARDS_CLIP_CASE     case id (E01, E04, ...)
//   VACARDS_CLIP_EXPECT   per-case expect.json
//   VACARDS_CLIP_RESULT   per-case result.json to write atomically
//   INKSCAPE_TEST_GUI=1   real GUI session, as for test_text-paste
//
// expect.* fields honoured here (all documented in testfiles/clipboard/README.md):
//   text_sha256/text_bytes        hash and byte length the document text must have
//   text_utf8                     literal document text when small enough
//   object_count                  number of new text objects the case must create
//   undo_steps                    number of Undo entries the paste must add
//   document_unchanged            true for reject/abort/no-op cases
//   require_nonempty_style        pasted text must carry real family/size defaults
//   import_object_id              failure_class import-object: element id that
//                                 must appear (only in the SVG/object payload)
//   forbidden_text_utf8           distinctive string from the LOSING
//                                 representation that must NOT become document
//                                 text (why an object/plain route really won)
//   max_elapsed_ms                upper bound on the wall time of one paste
//                                 command (the stalled-representation deadline)
//
// Environment policy (supervisor decision): when VACARDS_CLIP_SESSION is unset
// the binary skips, so a plain `ctest` of this target is not red. Once the
// session is set, every missing/partial/malformed input is a FAILURE, never a
// skip: the driver additionally rejects any run whose gtest JSON reports a
// skipped test.

#include <gtest/gtest.h>
#include <gtk/gtk.h>
#include <gtkmm/application.h>
#include <glibmm/main.h>
#include <gdkmm/display.h>
#include <gdkmm/clipboard.h>
#include <gdkmm/contentformats.h>

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "document.h"
#include "document-undo.h"
#include "inkscape.h"
#include "inkscape-application.h"
#include "object/sp-item.h"
#include "object/sp-root.h"
#include "object/sp-text.h"
#include "selection.h"
#include "style.h"
#include "text-editing.h"
#include "ui/clipboard.h"
#include "ui/tools/text-tool.h"
#include "inkscape-window.h"
#include "util/cast.h"
#include "xml/repr.h"

// Keep Win32 headers after the GTK/gtkmm headers, as elsewhere in the test
// suite: windows.h defines macros that collide with gtkmm identifiers.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

using namespace Inkscape;
namespace {

// ---------------------------------------------------------------------------
// Minimal JSON reader (the build has no nlohmann/json or json-glib; the
// driver's expect.json/ready.json/lease.json are small and driver-generated).
// ---------------------------------------------------------------------------
struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    Json const *find(std::string const &key) const
    {
        for (auto const &field : fields) {
            if (field.first == key) return &field.second;
        }
        return nullptr;
    }
    // Missing keys behave as JSON null; callers must check with find() when the
    // difference between "absent" and "null" matters.
    Json const &operator[](std::string const &key) const
    {
        static Json const null_value;
        auto const *found = find(key);
        return found ? *found : null_value;
    }
    bool is_null() const { return type == Type::Null; }
    std::string const *as_text() const { return type == Type::String ? &text : nullptr; }
    bool as_bool(bool fallback) const { return type == Type::Bool ? boolean : fallback; }
    long long as_int(long long fallback) const
    {
        return type == Type::Number ? static_cast<long long>(number) : fallback;
    }
};

class JsonParser
{
public:
    explicit JsonParser(std::string const &input) : _in(input) {}

    bool parse(Json &out)
    {
        skip_ws();
        if (!parse_value(out)) return false;
        skip_ws();
        if (_pos != _in.size()) {
            _error = "trailing data at offset " + std::to_string(_pos);
            return false;
        }
        return true;
    }

    std::string const &error() const { return _error; }

private:
    void skip_ws()
    {
        while (_pos < _in.size() && (_in[_pos] == ' ' || _in[_pos] == '\t' || _in[_pos] == '\n' || _in[_pos] == '\r')) {
            ++_pos;
        }
    }

    bool fail(std::string const &message)
    {
        if (_error.empty()) _error = message;
        return false;
    }

    bool parse_value(Json &out)
    {
        if (_pos >= _in.size()) return fail("unexpected end of input");
        char const c = _in[_pos];
        if (c == '{') return parse_object(out);
        if (c == '[') return parse_array(out);
        if (c == '"') {
            out.type = Json::Type::String;
            return parse_string(out.text);
        }
        if (c == 't') {
            if (_in.compare(_pos, 4, "true") != 0) return fail("invalid literal");
            _pos += 4;
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (c == 'f') {
            if (_in.compare(_pos, 5, "false") != 0) return fail("invalid literal");
            _pos += 5;
            out.type = Json::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (c == 'n') {
            if (_in.compare(_pos, 4, "null") != 0) return fail("invalid literal");
            _pos += 4;
            out.type = Json::Type::Null;
            return true;
        }
        return parse_number(out);
    }

    bool parse_object(Json &out)
    {
        out.type = Json::Type::Object;
        ++_pos; // {
        skip_ws();
        if (_pos < _in.size() && _in[_pos] == '}') {
            ++_pos;
            return true;
        }
        while (true) {
            skip_ws();
            std::string key;
            if (_pos >= _in.size() || _in[_pos] != '"') return fail("object key must be a string");
            if (!parse_string(key)) return false;
            skip_ws();
            if (_pos >= _in.size() || _in[_pos] != ':') return fail("missing ':'");
            ++_pos;
            skip_ws();
            Json value;
            if (!parse_value(value)) return false;
            out.fields.emplace_back(std::move(key), std::move(value));
            skip_ws();
            if (_pos < _in.size() && _in[_pos] == ',') {
                ++_pos;
                continue;
            }
            if (_pos < _in.size() && _in[_pos] == '}') {
                ++_pos;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }

    bool parse_array(Json &out)
    {
        out.type = Json::Type::Array;
        ++_pos; // [
        skip_ws();
        if (_pos < _in.size() && _in[_pos] == ']') {
            ++_pos;
            return true;
        }
        while (true) {
            skip_ws();
            Json value;
            if (!parse_value(value)) return false;
            out.items.push_back(std::move(value));
            skip_ws();
            if (_pos < _in.size() && _in[_pos] == ',') {
                ++_pos;
                continue;
            }
            if (_pos < _in.size() && _in[_pos] == ']') {
                ++_pos;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    static void append_utf8(std::string &out, unsigned codepoint)
    {
        if (codepoint <= 0x7f) {
            out.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    bool parse_hex4(unsigned &value)
    {
        if (_pos + 4 > _in.size()) return fail("truncated \\u escape");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            char const c = _in[_pos++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool parse_string(std::string &out)
    {
        ++_pos; // "
        while (_pos < _in.size()) {
            char const c = _in[_pos++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (_pos >= _in.size()) return fail("truncated escape");
            char const escape = _in[_pos++];
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned codepoint = 0;
                if (!parse_hex4(codepoint)) return false;
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (_pos + 6 > _in.size() || _in[_pos] != '\\' || _in[_pos + 1] != 'u') {
                        return fail("unpaired UTF-16 surrogate");
                    }
                    _pos += 2;
                    unsigned low = 0;
                    if (!parse_hex4(low)) return false;
                    if (low < 0xdc00 || low > 0xdfff) return fail("invalid low surrogate");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                }
                append_utf8(out, codepoint);
                break;
            }
            default: return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    bool parse_number(Json &out)
    {
        std::size_t const start = _pos;
        if (_pos < _in.size() && (_in[_pos] == '-' || _in[_pos] == '+')) ++_pos;
        while (_pos < _in.size() && ((_in[_pos] >= '0' && _in[_pos] <= '9') || _in[_pos] == '.' || _in[_pos] == 'e' ||
                                     _in[_pos] == 'E' || _in[_pos] == '+' || _in[_pos] == '-')) {
            ++_pos;
        }
        if (_pos == start) return fail("invalid value");
        std::string const token = _in.substr(start, _pos - start);
        char *end = nullptr;
        double const value = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0') return fail("invalid number: " + token);
        out.type = Json::Type::Number;
        out.number = value;
        return true;
    }

    std::string const &_in;
    std::size_t _pos = 0;
    std::string _error;
};

std::optional<Json> read_json_file(std::string const &path, std::string &error)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) {
        error = "cannot open " + path;
        return std::nullopt;
    }
    std::string data;
    char buffer[8192];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        data.append(buffer, read);
    }
    std::fclose(file);
    JsonParser parser(data);
    Json json;
    if (!parser.parse(json)) {
        error = "cannot parse " + path + ": " + parser.error();
        return std::nullopt;
    }
    return json;
}

// Safe string field access: absent or non-string values yield an empty string
// instead of dereferencing a null pointer.
std::string json_text_or_empty(Json const &object, std::string const &key)
{
    auto const *value = object.find(key);
    if (!value) return {};
    auto const *text = value->as_text();
    return text ? *text : std::string();
}

std::string json_escape(std::string const &input)
{
    std::string out;
    out.reserve(input.size() + 8);
    for (unsigned char const c : input) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

bool write_text_atomic(std::string const &path, std::string const &data, std::string &error)
{
    std::string const tmp = path + ".tmp." + std::to_string(static_cast<long long>(getpid()));
    FILE *file = std::fopen(tmp.c_str(), "wb");
    if (!file) {
        error = "cannot create " + tmp;
        return false;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), file) == data.size();
    if (std::fflush(file) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        std::remove(tmp.c_str());
        error = "cannot write " + tmp;
        return false;
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        error = "cannot rename " + tmp + " to " + path + ": " + std::strerror(errno);
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

std::string sha256_hex(std::string const &bytes)
{
    gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
                                                reinterpret_cast<guchar const *>(bytes.data()), bytes.size());
    std::string out = digest ? digest : "";
    g_free(digest);
    return out;
}

std::optional<std::string> env_value(char const *name)
{
    char const *value = std::getenv(name);
    if (!value) return std::nullopt;
    return std::string(value);
}

bool env_is_set(char const *name)
{
    char const *value = std::getenv(name);
    return value && *value;
}

// Parent process id for the lease ownership check. POSIX reports it directly;
// Windows reads it from a Toolhelp32 process snapshot. Any failure returns -1
// so the lease check fails closed instead of trusting an unverified value.
long long current_parent_pid()
{
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return -1;
    }
    PROCESSENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    long long parent_pid = -1;
    DWORD const self_pid = GetCurrentProcessId();
    if (Process32First(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == self_pid) {
                parent_pid = static_cast<long long>(entry.th32ParentProcessID);
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return parent_pid;
#else
    return static_cast<long long>(getppid());
#endif
}

// ---------------------------------------------------------------------------
// GUI helpers (same patterns as testfiles/src/text-paste-test.cpp)
// ---------------------------------------------------------------------------
InkscapeApplication &testApplication()
{
    static auto application = [] {
        Gtk::Application::wrap_in_search_entry2();
        g_setenv("INKSCAPE_APP_ID_TAG", "textpasteexternal", true);
        auto result = new InkscapeApplication();
        result->gio_app()->register_application();
        for (auto signal : {SIGSEGV, SIGABRT, SIGFPE, SIGILL}) {
            std::signal(signal, SIG_DFL);
        }
#ifndef _WIN32
        std::signal(SIGBUS, SIG_DFL);
#endif
        return result;
    }();
    return *application;
}

void drainMainContext()
{
    for (unsigned i = 0; i < 10000 && g_main_context_pending(nullptr); ++i) {
        g_main_context_iteration(nullptr, false);
    }
}

void pumpFor(int milliseconds)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        while (g_main_context_pending(nullptr)) {
            g_main_context_iteration(nullptr, false);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

bool pumpUntil(std::function<bool()> const &predicate, int timeout_ms)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!predicate()) {
        drainMainContext();
        if (std::chrono::steady_clock::now() >= deadline) {
            return predicate();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

Glib::RefPtr<Gdk::Clipboard> defaultClipboard()
{
    auto display = Gdk::Display::get_default();
    if (!display) return {};
    return display->get_clipboard();
}

std::vector<std::string> clipboardMimeTypes()
{
    std::vector<std::string> out;
    auto clipboard = defaultClipboard();
    if (!clipboard) return out;
    auto formats = clipboard->get_formats();
    if (!formats) return out;
    for (auto const &mime : formats->get_mime_types()) {
        out.emplace_back(mime);
    }
    return out;
}

Text::Layout::iterator iteratorAt(SPItem *text, unsigned index)
{
    auto const *layout = te_get_layout(text);
    auto it = layout->begin();
    for (unsigned i = 0; i < index; ++i) {
        if (!it.nextCharacter()) break;
    }
    return it;
}

std::set<std::string> objectIdsInDoc(SPDocument *document)
{
    std::set<std::string> out;
    std::function<void(XML::Node *)> walk = [&](XML::Node *node) {
        if (auto id = node->attribute("id")) out.insert(id);
        for (auto child = node->firstChild(); child; child = child->next()) walk(child);
    };
    walk(document->getReprDoc()->root());
    return out;
}

std::vector<SPText *> textsNotIn(SPDocument *document, std::set<std::string> const &before)
{
    std::vector<SPText *> out;
    for (auto const &id : objectIdsInDoc(document)) {
        if (before.count(id)) continue;
        if (auto *text = cast<SPText>(document->getObjectById(id))) out.push_back(text);
    }
    return out;
}

// Every string the document shows as TEXT: character data of any element plus
// the layout string of the created text objects. Attribute values are
// deliberately excluded: a marker that only lives in an attribute (e.g. the SVG
// payload's own data-* note) is not document text, while a literal insert of a
// markup payload always lands in character data.
std::string documentTextCorpus(SPDocument *document, std::vector<SPText *> const &created)
{
    std::string out;
    std::function<void(XML::Node *)> walk = [&](XML::Node *node) {
        if (node->type() == XML::NodeType::TEXT_NODE) {
            if (auto const *content = node->content()) out += content;
        }
        for (auto child = node->firstChild(); child; child = child->next()) walk(child);
    };
    walk(document->getReprDoc()->root());
    for (auto const *text : created) {
        if (text) out += sp_te_get_string_multiline(text);
    }
    return out;
}

// Failure classes the receiver implements. Kept in sync with the driver's
// FAILURE_CLASSES; an unknown class must fail loudly instead of falling through
// to the reject branch.
bool is_known_failure_class(std::string const &name)
{
    return name == "success" || name == "fallback-plain" || name == "import-object" ||
           name == "reject-no-mutation" || name == "abort-no-mutation" || name == "no-op";
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
constexpr char const *kFixtureSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg"
      xmlns:xlink="http://www.w3.org/1999/xlink"
      xmlns:sodipodi="http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd"
      width="480" height="360">
  <sodipodi:namedview id="namedview"/>
  <rect id="rect" x="280" y="40" width="60" height="40" style="fill:#123456"/>
</svg>)SVG";

struct Check {
    std::string name;
    bool ok;
    std::string detail;
};

class ExternalPasteTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        started_ = std::chrono::steady_clock::now();

        // Plain `ctest` of this target must not go red: without a driver session
        // there is nothing to receive. Everything below is a hard failure once
        // the session is set, because then the driver promised a full contract.
        if (!env_is_set("VACARDS_CLIP_SESSION")) {
            GTEST_SKIP() << "external clipboard harness not configured "
                            "(set VACARDS_CLIP_SESSION/EXPECT/RESULT/CASE or run "
                            "run-external-paste.py --allow-clipboard-takeover)";
        }

        RecordCheck("gui_enabled", env_is_set("INKSCAPE_TEST_GUI"),
                    "INKSCAPE_TEST_GUI=1 is required for the real paste route");
        ASSERT_TRUE(env_is_set("INKSCAPE_TEST_GUI")) << "the driver must enable the GUI test session";

        bool const all_env = env_is_set("VACARDS_CLIP_SESSION") && env_is_set("VACARDS_CLIP_CASE") &&
                             env_is_set("VACARDS_CLIP_EXPECT") && env_is_set("VACARDS_CLIP_RESULT");
        configured_ = all_env;
        for (char const *name : {"VACARDS_CLIP_SESSION", "VACARDS_CLIP_CASE", "VACARDS_CLIP_EXPECT",
                                 "VACARDS_CLIP_RESULT"}) {
            bool const present = env_is_set(name);
            RecordCheck(std::string("env_") + name, present, present ? "" : "missing required environment variable");
            EXPECT_TRUE(present) << "missing required environment variable " << name;
        }
        ASSERT_TRUE(all_env) << "the driver must provide the complete VACARDS_CLIP_* contract";
        session_dir_ = *env_value("VACARDS_CLIP_SESSION");
        case_id_ = *env_value("VACARDS_CLIP_CASE");
        expect_path_ = *env_value("VACARDS_CLIP_EXPECT");
        result_path_ = *env_value("VACARDS_CLIP_RESULT");

        std::string error;
        auto expect = read_json_file(expect_path_, error);
        ASSERT_TRUE(expect.has_value()) << error;
        expect_ = std::move(*expect);
        auto ready = read_json_file(session_dir_ + "/pub/ready.json", error);
        ASSERT_TRUE(ready.has_value()) << "readiness artifact missing: " << error;
        ready_ = std::move(*ready);
        lease_ = read_json_file(session_dir_ + "/lease.json", error); // diagnostic only

        ASSERT_TRUE(expect_.find("expect")) << "expect.json has no expect object";
        ASSERT_TRUE(expect_.find("case")) << "expect.json has no case object";
        ASSERT_TRUE(expect_.find("fixture")) << "expect.json has no fixture object";

        Json const &expect_body = expect_["expect"];
        expect_text_sha_ = json_text_or_empty(expect_body, "text_sha256");
        expect_text_bytes_ = expect_body.find("text_bytes") ? expect_body.find("text_bytes")->as_int(-1) : -1;
        expect_object_count_ =
            expect_body.find("object_count") ? expect_body.find("object_count")->as_int(-1) : -1;
        expect_undo_steps_ = expect_body.find("undo_steps") ? expect_body.find("undo_steps")->as_int(-1) : -1;
        document_unchanged_ =
            expect_body.find("document_unchanged") ? expect_body.find("document_unchanged")->as_bool(true) : true;
        require_nonempty_style_ = expect_body.find("require_nonempty_style")
                                      ? expect_body.find("require_nonempty_style")->as_bool(false)
                                      : false;
        // The manifest fixture hash/byte count, used to bind a success-class
        // expect.text_sha256/text_bytes to the fixture the publisher published.
        Json const &fixture_body = expect_["fixture"];
        expect_fixture_sha_ = json_text_or_empty(fixture_body, "sha256");
        expect_fixture_bytes_ =
            fixture_body.find("bytes") ? fixture_body.find("bytes")->as_int(-1) : -1;
        // import_object_id: the SVG/object representation must have imported this
        // element id; it exists only in that representation's payload.
        if (auto const *node = expect_body.find("import_object_id"); node && !node->is_null()) {
            import_object_id_ = json_text_or_empty(expect_body, "import_object_id");
        }
        // forbidden_text_utf8: a distinctive string of the LOSING representation
        // that must not become document text (string or list of strings).
        if (auto const *node = expect_body.find("forbidden_text_utf8"); node && !node->is_null()) {
            if (auto const *single = node->as_text()) {
                forbidden_texts_.push_back(*single);
            } else {
                for (auto const &entry : node->items) {
                    if (auto const *text = entry.as_text()) forbidden_texts_.push_back(*text);
                }
            }
        }
        // max_elapsed_ms: bound on one paste command's wall time; the stalled
        // representation must hit the single absolute deadline, not hang.
        if (auto const *node = expect_body.find("max_elapsed_ms");
            node && node->type == Json::Type::Number) {
            expect_max_elapsed_ms_ = node->as_int(-1);
        }
        if (auto const *literal = expect_body.find("text_utf8"); literal && !literal->is_null()) {
            has_literal_text_ = true;
            expected_literal_text_ = json_text_or_empty(expect_body, "text_utf8");
        }
        route_ = json_text_or_empty(expect_["case"], "route");
        requirement_ = json_text_or_empty(expect_["case"], "requirement");
        fixture_id_ = json_text_or_empty(expect_["fixture"], "id");
        failure_class_ = json_text_or_empty(expect_, "failure_class");
        nonce_ = json_text_or_empty(expect_, "nonce");
        if (auto const *formats = expect_.find("required_formats")) {
            for (auto const &format : formats->items) {
                if (auto const *text = format.as_text()) required_formats_.push_back(*text);
            }
        }
        ASSERT_FALSE(expect_text_sha_.empty()) << "expect.json must carry expect.text_sha256";
        ASSERT_GE(expect_text_bytes_, 0) << "expect.json must carry expect.text_bytes";
        ASSERT_GE(expect_object_count_, 0) << "expect.json must carry expect.object_count";
        ASSERT_GE(expect_undo_steps_, 0) << "expect.json must carry expect.undo_steps";
        ASSERT_FALSE(expect_fixture_sha_.empty()) << "expect.json must carry fixture.sha256";
        ASSERT_GE(expect_fixture_bytes_, 0) << "expect.json must carry fixture.bytes";
        ASSERT_FALSE(route_.empty()) << "expect.json must carry case.route";
        ASSERT_FALSE(failure_class_.empty()) << "expect.json must carry failure_class";
        ASSERT_FALSE(required_formats_.empty()) << "expect.json must carry required_formats";
        // Only the classes this receiver implements may drive the oracle; an
        // unknown spelling must not fall through to the reject branch.
        ASSERT_TRUE(is_known_failure_class(failure_class_))
            << "unsupported failure_class in expect.json: " << failure_class_;
        if (failure_class_ == "import-object") {
            ASSERT_FALSE(import_object_id_.empty())
                << "an import-object case must carry expect.import_object_id";
            ASSERT_FALSE(forbidden_texts_.empty())
                << "an import-object case must carry expect.forbidden_text_utf8";
        }

        // Publisher identity: the publication must belong to another process.
        auto const *ready_pid = ready_.find("pid");
        ASSERT_TRUE(ready_pid && ready_pid->type == Json::Type::Number) << "ready.json must carry a numeric pid";
        publisher_pid_ = ready_pid->as_int(-1);
        receiver_pid_ = static_cast<long long>(getpid());
        RecordCheck("publisher_pid_differs", publisher_pid_ > 0 && publisher_pid_ != receiver_pid_,
                    "publisher=" + std::to_string(publisher_pid_) + " receiver=" + std::to_string(receiver_pid_));
        ASSERT_GT(publisher_pid_, 0);
        ASSERT_NE(publisher_pid_, receiver_pid_) << "the clipboard owner must be a different process";
        if (!json_text_or_empty(ready_, "nonce").empty() && !nonce_.empty()) {
            EXPECT_EQ(json_text_or_empty(ready_, "nonce"), nonce_) << "ready.json nonce must match the driver nonce";
        }
        if (!json_text_or_empty(ready_, "fixture").empty() && !fixture_id_.empty()) {
            EXPECT_EQ(json_text_or_empty(ready_, "fixture"), fixture_id_)
                << "ready.json must describe this case's fixture";
        }
        if (!json_text_or_empty(ready_, "fixture_sha256").empty()) {
            EXPECT_EQ(json_text_or_empty(ready_, "fixture_sha256"), json_text_or_empty(expect_["fixture"], "sha256"))
                << "the publisher must have published the manifest fixture";
        }

        // The receiver is a direct child of the driver that owns the lease.
        char const *require_lease = std::getenv("VACARDS_CLIP_REQUIRE_LEASE");
        if (!require_lease || std::string(require_lease) != "0") {
            bool lease_ok = false;
            if (lease_) {
                auto const *lease_pid = lease_->find("pid");
                // Read the real parent once and require it to be a valid PID
                // before comparing: the helper returns -1 on failure, and a
                // malformed/missing lease pid also reads as -1, so an equality
                // must never accept -1 == -1. A 0 lease pid is invalid too.
                long long const parent_pid = current_parent_pid();
                long long const lease_pid_value =
                    (lease_pid && lease_pid->type == Json::Type::Number) ? lease_pid->as_int(-1) : -1;
                lease_ok = parent_pid > 0 && lease_pid_value > 0 && lease_pid_value == parent_pid;
            }
            RecordCheck("driver_owns_lease", lease_ok, "lease pid must be the parent driver process");
            EXPECT_TRUE(lease_ok) << "the session lease does not belong to the launching driver";
        }

        auto &application = testApplication();
        ASSERT_TRUE(application.gtk_app());
        ASSERT_TRUE(Application::exists());

        auto owned = SPDocument::createNewDocFromMem(std::string(kFixtureSvg));
        ASSERT_TRUE(owned);
        document_ = application.document_add(std::move(owned));
        ASSERT_TRUE(document_);
        document_->ensureUpToDate();
        desktop_ = application.createDesktop(document_, false, true);
        ASSERT_TRUE(desktop_);
    }

    void TearDown() override
    {
        if (document_) document_->setModifiedSinceSave(false);
        if (desktop_) testApplication().destroyDesktop(desktop_);
        desktop_ = nullptr;
        document_ = nullptr;
        if (configured_) write_result();
    }

    void RecordCheck(std::string name, bool ok, std::string detail = {})
    {
        checks_.push_back({std::move(name), ok, std::move(detail)});
    }

    void RecordCheckEqual(std::string name, std::string const &actual, std::string const &expected)
    {
        RecordCheck(name, actual == expected, "actual=" + actual + " expected=" + expected);
    }

    bool activatePasteAction()
    {
        auto *window = desktop_ ? desktop_->getInkscapeWindow() : nullptr;
        if (!window) {
            RecordCheck("paste_action_available", false, "no InkscapeWindow");
            return false;
        }
        bool const activated = window->activate_action("win.paste");
        pumpFor(200);
        if (document_) document_->ensureUpToDate();
        RecordCheck("paste_action_activated", activated, activated ? "" : "win.paste not activatable");
        return activated;
    }

    std::string xml() const { return sp_repr_save_buf(document_->getReprDoc()).raw(); }

    int undo_all(int max_steps)
    {
        int steps = 0;
        while (steps < max_steps) {
            document_->ensureUpToDate();
            if (!DocumentUndo::undo(document_)) break;
            ++steps;
        }
        document_->ensureUpToDate();
        return steps;
    }

    int redo_all(int max_steps)
    {
        int steps = 0;
        while (steps < max_steps) {
            document_->ensureUpToDate();
            if (!DocumentUndo::redo(document_)) break;
            ++steps;
        }
        document_->ensureUpToDate();
        return steps;
    }

    void checkpoint()
    {
        document_->ensureUpToDate();
        DocumentUndo::done(document_, Util::Internal::ContextString("External paste receiver fixture"), "draw-text");
        DocumentUndo::clearUndo(document_);
        DocumentUndo::clearRedo(document_);
        document_->setModifiedSinceSave(false);
    }

    void write_result();

    // Environment / manifest inputs
    bool configured_ = false;
    std::string session_dir_;
    std::string case_id_;
    std::string expect_path_;
    std::string result_path_;
    Json expect_;
    Json ready_;
    std::optional<Json> lease_;
    std::string route_;
    std::string requirement_;
    std::string fixture_id_;
    std::string failure_class_;
    std::string nonce_;
    std::vector<std::string> required_formats_;
    std::string expect_text_sha_;
    std::string expect_fixture_sha_;
    std::string expected_literal_text_;
    std::string import_object_id_;
    std::vector<std::string> forbidden_texts_;
    bool has_literal_text_ = false;
    long long expect_text_bytes_ = -1;
    long long expect_fixture_bytes_ = -1;
    long long expect_object_count_ = -1;
    long long expect_undo_steps_ = -1;
    long long expect_max_elapsed_ms_ = -1;
    bool document_unchanged_ = true;
    bool require_nonempty_style_ = false;
    long long publisher_pid_ = -1;
    long long receiver_pid_ = -1;

    // Live objects
    SPDocument *document_ = nullptr;
    SPDesktop *desktop_ = nullptr;

    // Observed evidence
    std::vector<Check> checks_;
    std::vector<std::string> formats_observed_;
    bool clipboard_remote_ = false;
    bool clipboard_remote_after_ = false;
    bool document_changed_ = false;
    long long observed_object_count_ = -1;
    long long observed_text_bytes_ = -1;
    std::string observed_text_sha_;
    long long observed_undo_steps_ = -1;
    long long observed_redo_steps_ = -1;
    long long paste_elapsed_ms_ = -1;
    std::optional<bool> direct_paste_returned_;
    std::string xml_sha_before_;
    std::string xml_sha_after_;
    std::chrono::steady_clock::time_point started_;
};

void ExternalPasteTest::write_result()
{
    std::string const xml_before = xml_sha_before_;
    std::string const xml_after = xml_sha_after_;
    long long const duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started_).count();
    bool const passed = !HasFailure();

    std::string out;
    out.reserve(4096);
    out += "{";
    out += "\"schema\":1,";
    out += "\"case_id\":\"" + json_escape(case_id_) + "\",";
    out += "\"requirement_id\":\"" + json_escape(requirement_) + "\",";
    out += "\"fixture_id\":\"" + json_escape(fixture_id_) + "\",";
    out += "\"route\":\"" + json_escape(route_) + "\",";
    out += std::string("\"outcome\":\"") + (passed ? "pass" : "fail") + "\",";
    out += "\"failure_class_expected\":\"" + json_escape(failure_class_) + "\",";
    out += "\"duration_ms\":" + std::to_string(duration_ms) + ",";
    out += "\"receiver_pid\":" + std::to_string(receiver_pid_) + ",";
    out += "\"publisher_pid\":" + std::to_string(publisher_pid_) + ",";
    out += "\"remote_preconditions\":{";
    out += std::string("\"is_local_before\":") + (clipboard_remote_ ? "false" : "true") + ",";
    out += std::string("\"is_local_after\":") + (clipboard_remote_after_ ? "false" : "true") + ",";
    out += "\"formats_observed\":[";
    for (std::size_t i = 0; i < formats_observed_.size(); ++i) {
        if (i) out += ",";
        out += "\"" + json_escape(formats_observed_[i]) + "\"";
    }
    out += "],\"required_formats\":[";
    for (std::size_t i = 0; i < required_formats_.size(); ++i) {
        if (i) out += ",";
        out += "\"" + json_escape(required_formats_[i]) + "\"";
    }
    out += "],\"nonce\":\"" + json_escape(nonce_) + "\"";
    if (auto const *change_count = ready_.find("change_count")) {
        out += ",\"ready_change_count\":" + std::to_string(change_count->as_int(-1));
    }
    out += "},";
    out += "\"expected\":{";
    out += "\"text_sha256\":\"" + json_escape(expect_text_sha_) + "\",";
    out += "\"text_bytes\":" + std::to_string(expect_text_bytes_) + ",";
    out += "\"object_count\":" + std::to_string(expect_object_count_) + ",";
    out += "\"undo_steps\":" + std::to_string(expect_undo_steps_) + ",";
    out += std::string("\"document_unchanged\":") + (document_unchanged_ ? "true" : "false");
    if (!import_object_id_.empty()) {
        out += ",\"import_object_id\":\"" + json_escape(import_object_id_) + "\"";
    }
    if (!forbidden_texts_.empty()) {
        out += ",\"forbidden_text_utf8\":[";
        for (std::size_t i = 0; i < forbidden_texts_.size(); ++i) {
            if (i) out += ",";
            out += "\"" + json_escape(forbidden_texts_[i]) + "\"";
        }
        out += "]";
    }
    if (expect_max_elapsed_ms_ > 0) {
        out += ",\"max_elapsed_ms\":" + std::to_string(expect_max_elapsed_ms_);
    }
    out += "},";
    out += "\"actual\":{";
    out += "\"text_sha256\":\"" + json_escape(observed_text_sha_) + "\",";
    out += "\"text_bytes\":" + std::to_string(observed_text_bytes_) + ",";
    out += "\"object_count\":" + std::to_string(observed_object_count_) + ",";
    out += "\"undo_steps\":" + std::to_string(observed_undo_steps_) + ",";
    out += "\"redo_steps\":" + std::to_string(observed_redo_steps_) + ",";
    out += "\"elapsed_ms\":" + std::to_string(paste_elapsed_ms_) + ",";
    out += std::string("\"document_changed\":") + (document_changed_ ? "true" : "false") + ",";
    out += "\"xml_sha256_before\":\"" + json_escape(xml_before) + "\",";
    out += "\"xml_sha256_after\":\"" + json_escape(xml_after) + "\"";
    if (direct_paste_returned_) {
        out += std::string(",\"direct_paste_returned\":") + (*direct_paste_returned_ ? "true" : "false");
    }
    out += "},";
    out += "\"checks\":[";
    for (std::size_t i = 0; i < checks_.size(); ++i) {
        if (i) out += ",";
        out += "{\"name\":\"" + json_escape(checks_[i].name) + "\",\"ok\":" +
               (checks_[i].ok ? "true" : "false") + ",\"detail\":\"" + json_escape(checks_[i].detail) + "\"}";
    }
    out += "]";
    out += "}\n";

    std::string error;
    if (!write_text_atomic(result_path_, out, error)) {
        std::fprintf(stderr, "receiver: cannot write result.json: %s\n", error.c_str());
    }
}

// ---------------------------------------------------------------------------
// The single case
// ---------------------------------------------------------------------------
TEST_F(ExternalPasteTest, RemoteClipboardPasteMatchesManifestOracle)
{
    // ----- preconditions: a live remote clipboard owned by another process ----
    auto clipboard = defaultClipboard();
    ASSERT_TRUE(clipboard) << "no default Gdk clipboard in this GUI session";
    bool const remote = pumpUntil([&] { return !clipboard->is_local(); }, 8000);
    clipboard_remote_ = remote;
    RecordCheck("clipboard_not_local", remote,
                "a fresh receiver must see a remote clipboard, never its own GDK claim");
    // Hard stop: pasting while the clipboard is still our own GDK claim would
    // make the case pass for the wrong reason (the largest false-result hazard).
    ASSERT_TRUE(remote) << "clipboard->is_local() is still true; refusing to paste our own content";

    formats_observed_ = clipboardMimeTypes();
    std::string formats_text;
    for (auto const &format : formats_observed_) formats_text += format + " ";
    RecordCheck("observed_formats", !formats_observed_.empty(), "observed: " + formats_text);
    ASSERT_FALSE(formats_observed_.empty()) << "the live clipboard advertises no formats";
    for (auto const &required : required_formats_) {
        bool const present = std::find(formats_observed_.begin(), formats_observed_.end(), required) !=
                             formats_observed_.end();
        RecordCheck("required_format:" + required, present, "observed: " + formats_text);
        EXPECT_TRUE(present) << "the live clipboard must offer " << required << " (observed: " << formats_text << ")";
    }

    // ----- fixture document checkpoint ---------------------------------------
    checkpoint();
    xml_sha_before_ = sha256_hex(xml());
    auto const objects_before = objectIdsInDoc(document_);

    // ----- drive the real Paste action on the requested route ----------------
    bool route_ready = false;
    if (route_ == "selector") {
        desktop_->getSelection()->set(document_->getObjectById("rect"));
        desktop_->setTool("/tools/select");
        route_ready = desktop_->getTool() != nullptr;
    } else if (route_ == "text-tool") {
        desktop_->getSelection()->clear();
        desktop_->setTool("/tools/text");
        auto *tool = dynamic_cast<UI::Tools::TextTool *>(desktop_->getTool());
        route_ready = tool && tool->textItem() == nullptr;
    } else {
        RecordCheck("route_supported", false, "unknown route: " + route_);
    }
    RecordCheck("route_ready:" + route_, route_ready, route_ready ? "" : "the route fixture is not installed");
    ASSERT_TRUE(route_ready) << "route setup failed for " << route_;

    // The paste command's wall time is measured around the real win.paste
    // action only (the reject-class direct probe below is a second, separate
    // command). A stalled representation must abort at the product's single
    // absolute deadline instead of hanging the receiver.
    auto const paste_started = std::chrono::steady_clock::now();
    ASSERT_TRUE(activatePasteAction()) << "the real win.paste action must be activatable";

    document_->ensureUpToDate();
    xml_sha_after_ = sha256_hex(xml());
    document_changed_ = xml_sha_after_ != xml_sha_before_;

    auto created = textsNotIn(document_, objects_before);
    observed_object_count_ = static_cast<long long>(created.size());
    paste_elapsed_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - paste_started).count();

    if (expect_max_elapsed_ms_ > 0) {
        bool const bounded = paste_elapsed_ms_ <= expect_max_elapsed_ms_;
        RecordCheck("paste_bounded_deadline", bounded,
                    "elapsed_ms=" + std::to_string(paste_elapsed_ms_) +
                        " max_elapsed_ms=" + std::to_string(expect_max_elapsed_ms_));
        EXPECT_LE(paste_elapsed_ms_, expect_max_elapsed_ms_)
            << "one paste command must return within its declared absolute deadline; the stall "
               "mechanism must be bounded, not a hang";
    }

    // The losing representation's distinctive text must not appear anywhere in
    // the document text: that is what proves the object route (not the plain or
    // rich alternative) won, and that no malformed rich payload was spliced in.
    if (!forbidden_texts_.empty()) {
        std::string const corpus = documentTextCorpus(document_, created);
        for (auto const &forbidden : forbidden_texts_) {
            bool const absent = corpus.find(forbidden) == std::string::npos;
            RecordCheck("forbidden_text_absent:" + forbidden, absent,
                        absent ? "" : "the losing representation's marker became document text");
            EXPECT_TRUE(absent) << "the losing representation's marker must never become document text: "
                                << forbidden;
        }
    }

    // A "success" case pastes the fixture bytes verbatim; a "fallback-plain" case
    // publishes a malformed/rich representation with a complete plain
    // alternative and must paste that alternative. Both are pasting classes; an
    // "import-object" case must import the SVG/object payload and create no text
    // object; a reject/abort/no-op class must not touch the document at all.
    bool const pasting_class = failure_class_ == "success" || failure_class_ == "fallback-plain";
    bool const import_class = failure_class_ == "import-object";
    RecordCheck("document_state_matches_declared", document_changed_ != document_unchanged_,
                "observed document_changed=" + std::string(document_changed_ ? "true" : "false") +
                    " declared document_unchanged=" + std::string(document_unchanged_ ? "true" : "false"));
    EXPECT_EQ(document_changed_, !document_unchanged_)
        << "the observed document state must match expect.document_unchanged";

    if (failure_class_ == "success") {
        // Bind the oracle to the fixture bytes: expect.json carries the fixture
        // hash and byte count, and the driver verified them against the file.
        RecordCheckEqual("fixture_sha256_binds_text", expect_text_sha_, expect_fixture_sha_);
        EXPECT_EQ(expect_text_sha_, expect_fixture_sha_)
            << "a success case must expect exactly the fixture bytes (hash)";
        RecordCheckEqual("fixture_bytes_bind_text_bytes", std::to_string(expect_text_bytes_),
                         std::to_string(expect_fixture_bytes_));
        EXPECT_EQ(expect_text_bytes_, expect_fixture_bytes_)
            << "a success case must expect exactly the fixture bytes (length)";
    }

    if (pasting_class) {
        ASSERT_EQ(created.size(), static_cast<std::size_t>(expect_object_count_))
            << "the number of new editable text objects must match the manifest oracle";
        ASSERT_FALSE(created.empty()) << "the " << failure_class_ << " class must create a text object";
        auto *pasted = created.front();
        std::string const text = sp_te_get_string_multiline(pasted);
        observed_text_sha_ = sha256_hex(text);
        observed_text_bytes_ = static_cast<long long>(text.size());
        RecordCheckEqual("text_sha256", observed_text_sha_, expect_text_sha_);
        EXPECT_EQ(observed_text_sha_, expect_text_sha_) << "pasted text hash must match the manifest oracle";
        RecordCheck("text_bytes", observed_text_bytes_ == expect_text_bytes_,
                    "actual=" + std::to_string(observed_text_bytes_) +
                        " expected=" + std::to_string(expect_text_bytes_));
        EXPECT_EQ(observed_text_bytes_, expect_text_bytes_) << "pasted text length must match the manifest oracle";
        if (has_literal_text_) {
            RecordCheck("text_utf8", text == expected_literal_text_, "actual==" + text);
            EXPECT_EQ(text, expected_literal_text_) << "pasted text must match the manifest literal";
        }
        if (failure_class_ == "fallback-plain") {
            // Under-cap malformed rich representation: the fallback must be the
            // COMPLETE plain alternative, never truncated or silently empty.
            RecordCheck("fallback_plain_complete",
                        observed_text_sha_ == expect_text_sha_ && observed_text_bytes_ == expect_text_bytes_,
                        "a fallback-plain case must paste the complete plain alternative");
            EXPECT_GT(observed_text_bytes_, 0) << "the plain fallback must not be empty";
            EXPECT_EQ(observed_text_bytes_, expect_text_bytes_) << "the plain fallback must not be truncated";
        }
        if (require_nonempty_style_) {
            auto const *style = sp_te_style_at_position(pasted, iteratorAt(pasted, 0));
            bool const styled = style && style->font_family.value() && *style->font_family.value() != '\0' &&
                                style->font_size.computed > 0.0;
            RecordCheck("editable_default_style", styled, "the new text needs a real family and a positive size");
            EXPECT_TRUE(styled) << "pasted plain text must use real Text tool defaults";
        }
    } else if (import_class) {
        // The object/SVG representation must win over any text alternative: the
        // imported element id exists only in the SVG/object payload, and no text
        // object may be created (a text object would mean the plain or rich
        // alternative won instead). The forbidden-text check above already
        // rejects a literal paste of any losing payload.
        ASSERT_FALSE(import_object_id_.empty()) << "expect.import_object_id is required for import-object";
        ASSERT_GT(expect_undo_steps_, 0) << "an imported object must be undoable";
        bool const preexisting = objects_before.count(import_object_id_) > 0;
        bool const imported = document_->getObjectById(import_object_id_.c_str()) != nullptr;
        RecordCheck("import_object_present", imported && !preexisting,
                    "id=" + import_object_id_ + (preexisting ? " (already existed before the paste)" : ""));
        EXPECT_FALSE(preexisting) << "the import oracle id must not exist before the paste";
        EXPECT_TRUE(imported) << "the SVG/object representation must be imported (missing id "
                              << import_object_id_ << ")";
        RecordCheck("no_text_object_from_alternative", created.empty(),
                    "created text objects=" + std::to_string(created.size()));
        EXPECT_TRUE(created.empty())
            << "an import-object case must not create a text object from a text alternative";
        RecordCheck("object_count", observed_object_count_ == expect_object_count_,
                    "actual=" + std::to_string(observed_object_count_) +
                        " expected=" + std::to_string(expect_object_count_));
        EXPECT_EQ(observed_object_count_, expect_object_count_)
            << "an import-object case must not create text objects";
    } else {
        // Reject/abort/no-op: nothing may change, no Undo entry may appear, and
        // the real ClipboardManager entry point must report that it pasted nothing.
        RecordCheck("document_unchanged", !document_changed_, "xml before/after must be identical");
        EXPECT_FALSE(document_changed_) << "a " << failure_class_ << " case must not mutate the document";
        RecordCheck("no_new_objects", created.empty(), "created=" + std::to_string(created.size()));
        EXPECT_TRUE(created.empty()) << "a " << failure_class_ << " case must not create objects";
        RecordCheck("object_count", observed_object_count_ == expect_object_count_,
                    "actual=" + std::to_string(observed_object_count_) +
                        " expected=" + std::to_string(expect_object_count_));
        EXPECT_EQ(observed_object_count_, expect_object_count_)
            << "the created-object count must match the manifest oracle";
        if (document_unchanged_) {
            // The unchanged document text is the empty string; the oracle hash
            // and length must say so (the driver enforces the same binding).
            RecordCheckEqual("unchanged_text_hash", expect_text_sha_, sha256_hex(std::string()));
            EXPECT_EQ(expect_text_sha_, sha256_hex(std::string()))
                << "a document_unchanged case must expect the unchanged (empty) text";
            EXPECT_EQ(expect_text_bytes_, 0) << "a document_unchanged case must expect zero text bytes";
        }
        bool const handled = UI::ClipboardManager::get()->paste(desktop_);
        document_->ensureUpToDate();
        direct_paste_returned_ = handled;
        RecordCheck("direct_paste_rejected", !handled, "ClipboardManager::paste returned " +
                                                           std::string(handled ? "true" : "false"));
        EXPECT_FALSE(handled) << "the rejected clipboard must not report a successful paste";
        EXPECT_EQ(sha256_hex(xml()), xml_sha_before_) << "the direct paste probe must not mutate the document";
    }

    // ----- exactly one Undo step for a paste, zero for a rejection ----------
    int const undo_steps = undo_all(8);
    observed_undo_steps_ = undo_steps;
    RecordCheck("undo_steps", undo_steps == expect_undo_steps_,
                "actual=" + std::to_string(undo_steps) + " expected=" + std::to_string(expect_undo_steps_));
    EXPECT_EQ(undo_steps, expect_undo_steps_) << "the paste must add exactly the manifest Undo count";
    EXPECT_EQ(sha256_hex(xml()), xml_sha_before_) << "undoing every step must restore the pre-paste document";

    int const redo_steps = redo_all(8);
    observed_redo_steps_ = redo_steps;
    EXPECT_EQ(redo_steps, expect_undo_steps_) << "Redo must restore the paste result";
    EXPECT_EQ(sha256_hex(xml()), xml_sha_after_) << "Redo must restore exactly the pasted document";

    // ----- the receiver never wrote the clipboard ----------------------------
    bool const remote_after = !clipboard->is_local();
    clipboard_remote_after_ = remote_after;
    RecordCheck("clipboard_not_local_after", remote_after, "the receiver must not claim the clipboard");
    EXPECT_TRUE(remote_after) << "the receiver process must never publish to the clipboard";
}

} // namespace
