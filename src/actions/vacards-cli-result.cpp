// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * VACards agent command line: result records, result channel, outcome latch
 * and the action specification registry (design 5.3-5.5).
 *
 * One process-wide state (channel, sequence counter, outcome latch, halt state)
 * and one process-wide spec registry, both guarded by a single mutex. Records
 * are serialized through Boost.JSON with a fixed key order. No exception is used
 * for control flow; this file never includes <boost/json/src.hpp> (it is
 * compiled once in src/io/artwork-library-manifest.cpp).
 */

#define BOOST_JSON_NO_LIB
#include "vacards-cli-result.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include <boost/json.hpp>

#include <glib.h>
#include <glib/gstdio.h>

namespace Inkscape::VACardsCli {

namespace {

enum class Sink
{
    Stderr,
    Stdout,
    File,
};

struct State
{
    Sink sink = Sink::Stderr;
    FILE *file = nullptr;
    unsigned seq = 0;
    bool rejected = false;
    bool failed = false;
    bool halt_on_error = true;
    bool halt_pending = false;
    bool export_veto = false;
    int chain_depth = 0;
};

// One process-wide state and one process-wide registry, both guarded by one mutex.
State &state()
{
    static State instance;
    return instance;
}

std::vector<ActionSpec const *> &registry()
{
    static std::vector<ActionSpec const *> instance;
    return instance;
}

std::mutex &mutex()
{
    static std::mutex instance;
    return instance;
}

boost::json::value json_string(std::string_view text)
{
    return boost::json::value(boost::json::string_view(text.data(), text.size()));
}

boost::json::array json_string_array(std::vector<std::string> const &values)
{
    boost::json::array array;
    array.reserve(values.size());
    for (auto const &value : values) {
        array.push_back(json_string(value));
    }
    return array;
}

// Integral limits are emitted as JSON integers; anything non-finite, fractional
// or outside the exact double range is emitted as a JSON double.
boost::json::value json_number(double value)
{
    if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 9007199254740992.0) {
        return boost::json::value(static_cast<std::int64_t>(value));
    }
    return boost::json::value(value);
}

} // namespace

std::string_view status_name(Status status)
{
    switch (status) {
    case Status::Ok:
        return "ok";
    case Status::Changed:
        return "changed";
    case Status::Unchanged:
        return "unchanged";
    case Status::Rejected:
        return "rejected";
    case Status::Cancelled:
        return "cancelled";
    case Status::Failed:
        return "failed";
    case Status::Uncertain:
        return "uncertain";
    case Status::Skipped:
        return "skipped";
    }
    return "";
}

std::string to_json_line(Record const &record, unsigned seq)
{
    boost::json::object object;

    object["schema"] = json_string(result_schema);
    object["seq"] = static_cast<std::uint64_t>(seq);
    object["action"] = json_string(record.action);
    object["params_text"] = json_string(record.params_text);

    boost::json::object params;
    for (auto const &pair : record.params) {
        params[boost::json::string_view(pair.first.data(), pair.first.size())] = json_string(pair.second);
    }
    object["params"] = std::move(params);

    object["status"] = json_string(status_name(record.status));
    object["reason"] = json_string(record.reason);
    object["message"] = json_string(record.message);
    object["mode"] = json_string(record.mode);
    object["dry_run"] = record.dry_run;

    if (record.document_path) {
        boost::json::object document;
        document["path"] = json_string(*record.document_path);
        object["document"] = std::move(document);
    } else {
        object["document"] = nullptr;
    }

    boost::json::object targets;
    targets["selected"] = static_cast<std::int64_t>(record.selected);
    targets["eligible"] = static_cast<std::int64_t>(record.eligible);
    targets["covered"] = static_cast<std::int64_t>(record.covered);
    boost::json::array excluded;
    excluded.reserve(record.excluded.size());
    for (auto const &exclusion : record.excluded) {
        boost::json::object entry;
        entry["id"] = json_string(exclusion.id);
        entry["reason"] = json_string(exclusion.reason);
        excluded.push_back(std::move(entry));
    }
    targets["excluded"] = std::move(excluded);
    object["targets"] = std::move(targets);

    object["created"] = json_string_array(record.created);
    object["modified"] = json_string_array(record.modified);
    object["deleted"] = json_string_array(record.deleted);
    object["selection_after"] = json_string_array(record.selection_after);
    object["undo"] = json_string(record.one_undo_step ? "one-step" : "none");
    object["metrics"] = record.metrics;
    object["warnings"] = json_string_array(record.warnings);

    if (record.error) {
        boost::json::object error;
        error["code"] = json_string(record.error->code);
        error["key"] = json_string(record.error->key);
        object["error"] = std::move(error);
    }
    if (!record.data.empty()) {
        object["data"] = record.data;
    }

    return boost::json::serialize(object);
}

Record make_skipped_record(std::string action, std::string params_text)
{
    Record record;
    record.action = std::move(action);
    record.params_text = std::move(params_text);
    record.status = Status::Skipped;
    record.reason = "halted-after-error";
    record.message = "Not run because an earlier action was rejected or failed.";
    return record;
}

bool open_result_channel(std::string const &target, std::string &error)
{
    std::lock_guard<std::mutex> const lock(mutex());
    State &s = state();

    if (target == "-") {
        if (s.sink == Sink::File) {
            if (s.file != nullptr) {
                fclose(s.file);
            }
            s.file = nullptr;
        }
        s.sink = Sink::Stdout;
        error.clear();
        return true;
    }

    FILE *file = g_fopen(target.c_str(), "wb");
    if (file == nullptr) {
        int const e = errno;
        error = "Cannot open result file '" + target + "': " + g_strerror(e);
        return false; // previous channel unchanged
    }

    if (s.file != nullptr) {
        fclose(s.file);
    }
    s.file = file;
    s.sink = Sink::File;
    error.clear();
    return true;
}

void emit(Record const &record)
{
    std::lock_guard<std::mutex> const lock(mutex());
    State &s = state();

    std::string const line = to_json_line(record, ++s.seq);

    // A short or failed write loses a result record the caller relies on;
    // never ignore it: mark the run failed (exit 4) and say so on stderr.
    bool write_ok = true;
    if (s.sink == Sink::File) {
        write_ok = fwrite(line.data(), 1, line.size(), s.file) == line.size();
        write_ok = (fwrite("\n", 1, 1, s.file) == 1) && write_ok;
        write_ok = (fflush(s.file) == 0) && write_ok;
    } else {
        FILE *out = (s.sink == Sink::Stdout) ? stdout : stderr;
        write_ok = fwrite(record_prefix.data(), 1, record_prefix.size(), out) == record_prefix.size();
        write_ok = (fwrite(line.data(), 1, line.size(), out) == line.size()) && write_ok;
        write_ok = (fwrite("\n", 1, 1, out) == 1) && write_ok;
        write_ok = (fflush(out) == 0) && write_ok;
    }
    if (!write_ok) {
        s.failed = true;
        if (!(s.sink == Sink::Stderr)) {
            fprintf(stderr, "VASTUDIO-RESULT-ERROR could not write the result record (seq %u)\n", s.seq);
        }
    }

    if (record.status == Status::Rejected) {
        s.rejected = true;
    }
    if (record.status == Status::Failed || record.status == Status::Uncertain) {
        s.failed = true;
    }

    if (s.halt_on_error
        && (record.status == Status::Rejected || record.status == Status::Failed
            || record.status == Status::Uncertain)) {
        s.halt_pending = true;
        s.export_veto = true;
    }
}

void note_rejection()
{
    std::lock_guard<std::mutex> const lock(mutex());
    state().rejected = true;
}

int exit_status()
{
    std::lock_guard<std::mutex> const lock(mutex());
    State const &s = state();
    return s.failed ? 4 : (s.rejected ? 3 : 0);
}

void set_halt_on_error(bool halt)
{
    std::lock_guard<std::mutex> const lock(mutex());
    state().halt_on_error = halt;
}

bool halt_on_error()
{
    std::lock_guard<std::mutex> const lock(mutex());
    return state().halt_on_error;
}

bool halt_pending()
{
    std::lock_guard<std::mutex> const lock(mutex());
    return state().halt_pending;
}

void begin_chain()
{
    std::lock_guard<std::mutex> const lock(mutex());
    state().halt_pending = false;
}

void begin_document()
{
    std::lock_guard<std::mutex> const lock(mutex());
    state().export_veto = false;
}

bool export_vetoed()
{
    std::lock_guard<std::mutex> const lock(mutex());
    return state().export_veto;
}

CommandLineChainScope::CommandLineChainScope()
{
    std::lock_guard<std::mutex> const lock(mutex());
    ++state().chain_depth;
}

CommandLineChainScope::~CommandLineChainScope()
{
    std::lock_guard<std::mutex> const lock(mutex());
    --state().chain_depth;
}

bool command_line_chain_active()
{
    std::lock_guard<std::mutex> const lock(mutex());
    return state().chain_depth > 0;
}

void reset_for_testing()
{
    std::lock_guard<std::mutex> const lock(mutex());
    State &s = state();
    if (s.file != nullptr) {
        fclose(s.file);
    }
    s = State{};
    // the registry is intentionally kept
}

void register_action_spec(ActionSpec const &spec)
{
    std::lock_guard<std::mutex> const lock(mutex());
    auto &specs = registry();
    for (auto const *existing : specs) {
        if (existing->name == spec.name) {
            return;
        }
    }
    specs.push_back(&spec);
}

std::vector<ActionSpec const *> registered_action_specs()
{
    std::lock_guard<std::mutex> const lock(mutex());
    std::vector<ActionSpec const *> specs = registry();
    std::sort(specs.begin(), specs.end(), [](ActionSpec const *a, ActionSpec const *b) { return a->name < b->name; });
    return specs;
}

ActionSpec const *find_action_spec(std::string_view name)
{
    std::lock_guard<std::mutex> const lock(mutex());
    for (auto const *spec : registry()) {
        if (spec->name == name) {
            return spec;
        }
    }
    return nullptr;
}

boost::json::object describe_action(ActionSpec const &spec)
{
    boost::json::object object;
    object["name"] = json_string(spec.name);
    object["mode"] = json_string(spec.mode);
    object["summary"] = json_string(spec.summary);

    boost::json::array params;
    params.reserve(spec.params.size());
    for (auto const &param : spec.params) {
        boost::json::object entry;
        entry["key"] = json_string(param.key);
        entry["type"] = json_string(type_name(param.type));
        if (std::string_view const unit = stored_unit(param.type); !unit.empty()) {
            entry["unit"] = json_string(unit);
        }
        entry["required"] = param.required;
        if (!param.default_value.empty()) {
            entry["default"] = json_string(param.default_value);
        }
        if (param.min) {
            entry["min"] = json_number(*param.min);
        }
        if (param.max) {
            entry["max"] = json_number(*param.max);
        }
        if (param.type == ParamType::Choice) {
            boost::json::array choices;
            choices.reserve(param.choices.size());
            for (auto const &choice : param.choices) {
                choices.push_back(json_string(choice));
            }
            entry["choices"] = std::move(choices);
        }
        if (!param.help.empty()) {
            entry["help"] = json_string(param.help);
        }
        params.push_back(std::move(entry));
    }
    object["params"] = std::move(params);

    return object;
}

} // namespace Inkscape::VACardsCli

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4 :
