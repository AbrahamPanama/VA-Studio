// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/vacards-cli-files.h"
#include "actions/vacards-cli-fault.h"
#include "io/vacards-cli-import.h"
#include "io/vacards-cli-intake.h"
#include "io/document-file-transaction.h"
#include "io/existing-file-replacement.h"
#include "io/export-color-profiles.h"
#include "io/tiff-export.h"
#include "helper/png-write.h"
#include "colors/color.h"
#include "display/drawing.h"
#include "extension/internal/cairo-renderer.h"
#include "extension/internal/cairo-render-context.h"
#include "event-log.h"
#include "object/sp-root.h"
#include "object/sp-page.h"
#include "page-manager.h"
#include "path-chemistry.h"
#include "selection.h"
#include "util/units.h"
#include "xml/repr.h"
#include "xml/rebase-hrefs.h"
#include <boost/json.hpp>
#include <glib/gstdio.h>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace Inkscape::VACardsCli {
namespace {
using namespace boost::json;
namespace fs = std::filesystem;
std::string str(object const &p, char const *k, char const *fallback="") { auto v=p.if_contains(k); return v ? std::string(v->as_string()) : fallback; }
bool flag(object const &p, char const *k) { auto v=p.if_contains(k); return v && v->as_bool(); }
double number(object const &p,char const *k,double fallback) { auto v=p.if_contains(k); return v ? value_to<double>(*v) : fallback; }
double px(value const &v) { auto const &l=v.as_object(); return Util::Quantity::convert(value_to<double>(l.at("value")),str(l,"unit"),"px"); }
object version(IO::ExpectedFileVersion const &v) { return {{"identity",v.identity},{"sha256",v.sha256},{"bytes",v.bytes}}; }
IO::ExpectedFileVersion version(value const &v) { auto const &p=v.as_object(); return {str(p,"identity"),str(p,"sha256"),value_to<std::uint64_t>(p.at("bytes"))}; }
bool same(IO::ExpectedFileVersion const &a,IO::ExpectedFileVersion const &b) { return a.identity==b.identity && a.sha256==b.sha256 && a.bytes==b.bytes; }
Record reject(Record r,std::string code,std::string message,Status status=Status::Rejected) {
    r.status=status; r.reason=code; r.message=message; r.error=ParseError{code,{},message}; return r;
}
Record reject_intake(Record r, IntakeError const &error) {
    r.error_details = error.details;
    r.error_retryable = error.retryable;
    return reject(std::move(r), error.code, error.message);
}
bool cancelled(DispatchContext const &c) { return c.cancelled && c.cancelled(); }
FileLoadOptions options(object const &p) {
    FileLoadOptions o{str(p,"format","auto"),str(p,"resource-policy"),str(p,"font-policy"),{}};
    if (auto pages=p.if_contains("pages")) for (auto const &v:pages->as_array()) o.pages.push_back(value_to<unsigned>(v));
    return o;
}
struct Scratch {
    std::string directory;
    Scratch() { auto p=g_dir_make_tmp("vacards-cli-M2-XXXXXX",nullptr); if (!p) throw std::runtime_error("Cannot create private rendering directory"); directory=p; g_free(p); }
    ~Scratch() { std::error_code ec; fs::remove_all(directory,ec); }
    std::string path(char const *leaf) const { return directory+"/"+leaf; }
};
std::string read_payload(std::string const &path) {
    std::error_code ec; auto size=fs::file_size(path,ec);
    if (ec || size>(512ull<<20)) throw std::runtime_error("Payload exceeds 512 MiB or cannot be read");
    std::ifstream in(path,std::ios::binary); std::string bytes(size,'\0');
    if (!in.read(bytes.data(),size)) throw std::runtime_error("Cannot read rendered payload"); return bytes;
}
// Resolve one immutable profile; the preference-writing select() API is never used.
struct ProfileError { std::string code, message; };
bool resolve_profile(object const &params,Grants const &grants,IO::PreparedExportProfile &out, ProfileError &failure, FileServiceTestHooks const *hooks) {
    auto &error=failure.message; auto &code=failure.code;
    code="profile-invalid";
    object p{{"id","default"}}; if (auto v=params.if_contains("profile")) p=v->as_object();
    auto id=str(p,"id");
    if (id=="file") {
        auto path=str(p,"path"), hash=str(p,"sha256");
        if (path.empty() || hash.empty()) { error="Custom profile requires path and sha256"; return false; }
        auto access=inspect_command_path(path,grants);
        if(access.state!="granted") { code=(access.state=="unavailable" || access.state=="missing") ? "resource-unavailable" : access.state=="invalid" ? "profile-invalid" : "read-grant-denied"; error="Custom profile is not granted"; return false; }
        auto input=read_admitted(access,IO::EXPORT_PROFILE_MAX_SIZE);
        if(!input.error.empty()) { code=input.error; error="Custom profile is unavailable"; return false; }
        if(input.sha256!=hash) { code="stale-dependency"; error="Custom profile hash changed"; return false; }
        if(!IO::ExportColorProfiles::read_bytes(access.path,{input.bytes.begin(),input.bytes.end()},out.profile,error)) return false;
    } else {
        if (p.contains("path") || p.contains("sha256")) { error="Only file profiles accept path and sha256"; return false; }
        if (id=="srgb") out.profile=IO::ExportColorProfiles::srgb();
        else {
            auto override=g_getenv("INKSCAPE_VACARDS_TIFF_ICC_PROFILE");
            if(override && *override) {
                auto access=inspect_command_path(override,grants);
                if(access.state!="granted") {code=(access.state=="unavailable" || access.state=="missing") ? "resource-unavailable" : access.state=="invalid" ? "profile-invalid" : "read-grant-denied";error="ICC environment override is not granted";return false;}
                auto input=read_admitted(access,IO::EXPORT_PROFILE_MAX_SIZE);
                if(!input.error.empty()) {code=input.error;error="ICC environment override is unavailable";return false;}
                if(!IO::ExportColorProfiles::read_bytes(access.path,{input.bytes.begin(),input.bytes.end()},out.profile,error)) return false;
            } else out.profile=hooks && hooks->default_profiles ? hooks->default_profiles->resolve("",out.notice) : IO::ExportColorProfiles().resolve("",out.notice);
        }
    }
    out.transform=IO::export_color_transform(out.profile.bytes,error);
    return bool(out.transform);
}
// Use native Cairo PDF traversal, with a request-owned context and no extension parameter writes.
bool render_pdf(SPDocument *doc,std::string const &path,double dpi) {
    using namespace Extension::Internal;
    auto root=doc->getRoot(); Drawing drawing; unsigned key=SPItem::display_key_new(1);
    drawing.setRoot(root->invoke_show(drawing,key,SP_ITEM_SHOW_DISPLAY)); drawing.setExact();
    struct Hide { SPRoot *root; unsigned key; ~Hide(){root->invoke_hide(key);} } hide{root,key};
    CairoRenderer renderer; auto ctx=renderer.createContext();
    ctx.setPDFLevel(0); ctx.setTextToPath(false); ctx.setOmitText(false); ctx.setFilterToBitmap(true); ctx.setBitmapResolution(dpi);
    return ctx.setPdfTarget((">"+path).c_str()) && renderer.setupDocument(&ctx,doc,root) &&
        renderer.renderPages(&ctx,doc,false) && ctx.finish();
}
Record publish(Record r, Request const &request,DispatchContext &context,FileState &state, std::string const &payload, Grants const &grants, FileServiceTestHooks const *hooks) {
    auto path=str(request.params,"path");
    auto destination=inspect_write_destination(path,grants);
    if(destination.state!="granted") return reject(r,"unsafe-destination","Destination changed before publication.");
    path=destination.path;
    auto digest=g_compute_checksum_for_data(G_CHECKSUM_SHA256,reinterpret_cast<guchar const *>(payload.data()),payload.size());
    r.data["bytes"]=payload.size(); r.data["sha256"]=digest; g_free(digest);
    auto writer=[&](FILE *stream) { if (std::fwrite(payload.data(),1,payload.size(),stream)!=payload.size()) throw std::runtime_error("Payload write failed"); };
    std::string outcome,error,recovery; bool cleanup=true;
    object durability;
    if (cancelled(context)) return reject(r,"cancelled","Cancelled before publication preparation.",Status::Cancelled);
    if (flag(request.params,"overwrite")) {
        IO::ExistingFileOptions opts;
        if (hooks) opts.stage_observer=hooks->replacement_stage;
        // The native observer is a production cancellation check, not fault injection.
        opts.cancelled=[&] { return cancelled(context); };
        auto result=IO::replace_existing_local_file(path,writer,version(request.params.at("expected-version")),opts);
        using E=IO::ExistingFileOutcome;
        if(result.outcome==E::Cancelled) return reject(r,"cancelled",result.error,Status::Cancelled);
        if(result.outcome==E::Unavailable) return reject(r,"publication-unavailable",result.error);
        outcome=result.outcome==E::Published ? "Published" : result.outcome==E::Conflict ? "Conflict" :
            result.outcome==E::Unsupported ? "Unsupported" : result.outcome==E::Uncertain ? "Uncertain" : "Failed";
        error=result.error; recovery=result.recovery_path;
        cleanup=recovery.empty();
        durability={{"file_sync","native-replacement"},{"parent_sync","platform-dependent"}};
    } else {
        namespace T=IO::DocumentTransaction;
        auto calls=T::make_platform_system_calls(); T::FailureKind failure{};
        auto target=fs::u8path(path);
        auto tx=T::NewDocumentFile::create({path,target.filename().string(),target.parent_path().string()},
            hooks && hooks->calls ? *hooks->calls : *calls,failure,error,false,
            hooks ? hooks->new_stage : std::function<bool(unsigned)>{});
        if (!tx) outcome=failure==T::FailureKind::DestinationExists ? "Conflict" : failure==T::FailureKind::Unsupported ? "Unsupported" : "Failed";
        else if (!tx->write(writer,failure,error) || !tx->seal(failure,error)) outcome="Failed";
        else if (cancelled(context)) { tx->abort(); return reject(r,"cancelled","Cancelled before publication.",Status::Cancelled); }
        else {
            auto result=tx->publish(); outcome=T::to_string(result.status); error=result.error; recovery=result.recovery_path;
            cleanup=result.staging_cleanup_ok;
            durability={{"file_sync_attempted",result.file_sync_attempted},{"file_sync_supported",result.file_sync_supported},
              {"file_sync_ok",result.file_sync_ok},{"parent_sync_attempted",result.parent_sync_attempted},
              {"parent_sync_supported",result.parent_sync_supported},{"parent_sync_ok",result.parent_sync_ok}};
        }
    }
    r.data["publication"]=object{{"outcome",outcome},{"destination",path},{"error",error},
        {"durability",durability},{"cleanup_ok",cleanup},{"recovery_path",recovery},
        {"recovery_availability",recovery.empty() ? "none" : "unverified"}};
    auto observed=IO::inspect_existing_file_version(path);
    if (observed.version) r.data["destination_version"]=version(*observed.version);
    if (outcome=="Published") {
        r.status=Status::Changed; r.publication="published"; r.publication_persisted=true;
        if (!cleanup || !error.empty()) r.warnings.emplace_back("publication-cleanup");
        if (request.command=="file.save" && document_stamp(context.document).revision==r.revision_before) {
            context.document->setDocumentFilename(path.c_str()); context.document->setModifiedSinceSave(false);
            auto log=context.document->get_event_log(); log->rememberFileSave(log->getCurrEventSerial());
            r.data["saved_revision"]=r.revision_before;
        }
    } else if (outcome=="Uncertain") {
        state.writes_blocked=true; state.reconciliation=r.data.at("publication").as_object();
        state.reconciliation["acknowledged"]=false; r.publication="uncertain";
        r=reject(r,"publication-uncertain","Publication is unconfirmed. Reconcile the destination before any further write.",Status::Uncertain);
    } else if (outcome=="Conflict") r=reject(r,"publication-conflict","Destination version differs or destination already exists.");
    else if (outcome=="Unsupported") r=reject(r,"publication-unsupported",error);
    else r=reject(r,"publication-failed",error,Status::Failed);
    r.data["dirty"]=context.document->isModifiedSinceSave();
    return r;
}
}
static Record execute_file_impl(Request const &request, DispatchContext &context, FileState &state, Grants const &grants, FileServiceTestHooks const *hooks)
{
    Record r; r.action=request.command; r.dry_run=request.dry_run;
    if (context.document) { auto s=document_stamp(context.document); r.document_id=s.id; r.revision_before=r.revision_after=s.revision; }
    auto const &p=request.params;
    bool publication_started=false;
    try {
        if (cancelled(context)) return reject(r,"cancelled","Cancelled before preparation.",Status::Cancelled);
        cli_fault_throw("file.before-service");
        bool lifecycle=request.command=="file.new" || request.command=="file.open" || request.command=="file.close";
        if (lifecycle) {
            if (context.document && context.document->isModifiedSinceSave() && !flag(p,"discard"))
                return reject(r,"dirty-document","Save the active document or explicitly discard it.");
            if (request.command=="file.close") {
                if (state.writes_blocked && p.contains("reconcile")) {
                    auto const &rec=p.at("reconcile").as_object(); auto dest=str(rec,"destination");
                    auto expected_dest=str(state.reconciliation,"destination");
                    bool same_destination=dest==expected_dest;
#ifdef _WIN32
                    if (!same_destination) {
                        // Compare admitted final entry spellings, not slash/case/8.3
                        // aliases supplied by the caller. Identity alone would also
                        // accept another hard link, which is not this destination.
                        auto actual=inspect_write_destination(dest,grants);
                        auto expected=inspect_write_destination(expected_dest,grants);
                        same_destination=actual.state=="granted" && expected.state=="granted" && actual.path==expected.path;
                        if (same_destination) dest=expected.path;
                    }
#endif
                    if (!flag(p,"discard") || !flag(rec,"acknowledge") || !same_destination)
                        return reject(r,"reconciliation-required","Reconciliation must acknowledge the uncertain destination and discard the document.");
                    if (inspect_write_destination(dest,grants).state!="granted") return reject(r,"write-grant-denied","Reconciliation destination is not granted.");
                    auto actual=IO::inspect_existing_file_version(dest);
                    if (!actual.version || !same(*actual.version,version(rec.at("observed-version"))))
                        return reject(r,"reconciliation-required","Observed destination version does not match disk.");
                    if (!request.dry_run) state.reconciliation["acknowledged"]=true;
                } else if (state.writes_blocked) return reject(r,"reconciliation-required","Explicit uncertainty reconciliation is required before closing.");
                bool had=context.document; r.data={{"closed",had},{"discarded",had && flag(p,"discard")},{"validation_level","preflight"}};
                if (!request.dry_run && had) { if (state.before_document_retire) state.before_document_retire(); context.document=nullptr; context.selection=nullptr; state.document.reset(); state.read_only=false; }
                r.data["session_revision"]=context.session_revision;
                r.status=request.dry_run ? Status::Ok : had ? Status::Changed : Status::Unchanged;
                if (!request.dry_run) r.document_id.clear(); return r;
            }
            IntakeResult intake;
            if (request.command=="file.new") {
                auto w=px(p.at("width")),h=px(p.at("height"));
                if (!std::isfinite(w) || !std::isfinite(h) || w<=0 || h<=0 || w>1000000 || h>1000000)
                    return reject(r,"out-of-range","Dimensions must be positive and at most 1000000 CSS px.");
                intake.document=SPDocument::createNewDoc(nullptr,true);
                DocumentUndo::setUndoSensitive(intake.document.get(),false);
                intake.document->setWidthAndHeight(Util::Quantity(w,"px"),Util::Quantity(h,"px"));
                intake.document->setViewBox(Geom::Rect::from_xywh(0,0,w,h)); intake.document->ensureUpToDate();
                DocumentUndo::setUndoSensitive(intake.document.get(),true);
                intake.document->setModifiedSinceSave(true);
                r.data={{"width",report_length(w,context.preferred_unit)},{"height",report_length(h,context.preferred_unit)},
                        {"units",context.preferred_unit},{"dirty",true},{"validation_level","computed"}};
            } else {
                auto path=str(p,"path");
                auto load_options=options(p);
                if (hooks) load_options.observe_for_testing=hooks->intake_observer;
                intake=load_editable_document(path,grants,load_options);
                if (intake.error) return reject_intake(std::move(r), *intake.error);
                if (auto fonts=intake.report.if_contains("fonts"); fonts && !fonts->as_array().empty()) r.warnings.emplace_back("font-substitution");
                r.data={{"intake",intake.report},{"source_version",intake.source_version},
                        {"dirty",intake.document->isModifiedSinceSave()},{"validation_level","computed"}};
                if (state.writes_blocked && !flag(state.reconciliation,"acknowledged"))
                    return reject(r,"reconciliation-required","Reconcile the uncertain destination before reopening.");
            }
            if (serialize(r.data).size()>(8u<<20)-65536) return reject(Record{},"engine-limit","Intake report exceeds response limit.");
            if (cancelled(context)) return reject(r,"cancelled","Cancelled before document swap.",Status::Cancelled);
            if (!request.dry_run) {
                if (state.document && state.before_document_retire) state.before_document_retire();
                state.document=std::move(intake.document); state.read_only=false; state.provenance=intake.report;
                context.document=state.document.get(); context.selection=context.document->getSelection();
                if (request.command=="file.open" && flag(state.reconciliation,"acknowledged")) { state.writes_blocked=false; state.reconciliation.clear(); }
                auto s=document_stamp(context.document); r.document_id=s.id; r.revision_after=s.revision;
                r.data["document_id"]=s.id; r.data["revision"]=s.revision;
                r.status=Status::Changed;
            }
            r.data["session_revision"]=context.session_revision; return r;
        }
        if (!context.document) return reject(r,"no-document","This command requires a document.");
        if (state.read_only && request.command!="file.export") return reject(r,"document-read-only","Explicitly open an editable document first.");
        if (request.command=="file.import") return import_document(request,context,grants,hooks ? hooks->intake_observer : IntakeObserver{});
        if (state.writes_blocked) return reject(r,"writes-blocked","A prior uncertain publication must be reconciled before another write.");
        auto path=str(p,"path"); auto access=inspect_write_destination(path,grants);
        if (access.state!="granted") return reject(r,access.state=="unavailable" ? "publication-unavailable" : access.state=="ungranted" ? "write-grant-denied" : "unsafe-destination","Destination must be a granted stable local file without symlink components.");
        path=access.path;
        bool overwrite=flag(p,"overwrite");
        if (overwrite && !p.contains("expected-version")) return reject(r,"expected-version-required","Overwrite requires the observed destination identity, size and SHA-256.");
        auto observed=IO::inspect_existing_file_version(path);
        if (observed.version) r.data["destination_version"]=version(*observed.version);
        if(observed.outcome==IO::ExistingFileOutcome::Unavailable) return reject(r,"publication-unavailable",observed.error);
        if ((!overwrite && observed.version) || (overwrite && (!observed.version || !same(*observed.version,version(p.at("expected-version"))))))
            return reject(r,"publication-conflict","Destination exists or its expected version differs.");
        std::string format=request.command=="file.save" ? "svg" : str(p,"format");
        bool const prevent_white_clipping = flag(p,"prevent-white-clipping");
        bool const white_clipping_transparent = flag(p,"white-clipping-transparent");
        bool const clean_edges = flag(p,"clean-edges");
        bool const hard_edges = flag(p,"hard-edges");
        if (request.command=="file.export" &&
            (prevent_white_clipping || white_clipping_transparent || clean_edges || hard_edges) && format!="tiff")
            return reject(r,"invalid-argument","RIP options apply to TIFF export only.");
        if (request.command=="file.export" && white_clipping_transparent && !prevent_white_clipping)
            return reject(r,"invalid-argument","white-clipping-transparent requires prevent-white-clipping.");
        // Admit an export's raster size before the generic snapshot budget so the
        // refusal identifies raster-export and no rendering snapshot is allocated.
        if (request.command=="file.export" && (format=="png" || format=="tiff")) {
            unsigned scopes=p.contains("page")+p.contains("ids")+p.contains("area")+p.contains("drawing");
            if (scopes!=1 || (p.contains("drawing") && !flag(p,"drawing")))
                return reject(r,"invalid-target","Choose exactly one page, ids, area or drawing scope.");
            Geom::OptRect source_area;
            if (p.contains("page")) {
                auto index=value_to<unsigned>(p.at("page")); auto &manager=context.document->getPageManager();
                if (manager.getPageCount()) { auto page=manager.getPage(index-1); if (page) source_area=page->getDocumentRect(); }
                else if (index==1) source_area=Geom::Rect::from_xywh(0,0,context.document->getWidth().value("px"),context.document->getHeight().value("px"));
            } else if (p.contains("area")) {
                auto const &a=p.at("area").as_object(); auto w=px(a.at("width")),h=px(a.at("height"));
                if (w>0 && h>0) source_area=Geom::Rect::from_xywh(px(a.at("x")),px(a.at("y")),w,h);
            } else if (p.contains("ids")) {
                std::set<std::string> seen;
                for (auto const &id:p.at("ids").as_array()) {
                    std::string name(id.as_string()); auto item=cast<SPItem>(context.document->getObjectById(name.c_str()));
                    if (!item || item==context.document->getRoot() || !seen.insert(name).second)
                        return reject(r,"invalid-target","IDs must name unique graphical objects.");
                    source_area.unionWith(item->documentVisualBounds());
                }
            } else source_area=context.document->getRoot()->documentVisualBounds();
            if (!source_area || !std::isfinite(source_area->width()) || !std::isfinite(source_area->height()) ||
                source_area->width()<=0 || source_area->height()<=0)
                return reject(r,"invalid-target","Scope has no finite positive bounds.");
            double dpi=number(p,"dpi",96);
            auto width=std::max(1.0,std::floor(source_area->width()*dpi/96+0.5));
            auto height=std::max(1.0,std::floor(source_area->height()*dpi/96+0.5));
            if (width*height>103680000)
                return reject(r,"out-of-range","Raster export is limited to 103.68 MP (12 x 24 in at 600 dpi).");
            if (auto error=admit_raster_export_memory(static_cast<std::uint64_t>(width)*static_cast<std::uint64_t>(height)))
                return reject_intake(std::move(r),*error);
        }
        auto prepared_snapshot=prepare_file_snapshot(*context.document,grants,
            request.command=="file.save" ? str(p,"embedding-policy") : "embed");
        if (prepared_snapshot.error) return reject_intake(std::move(r), *prepared_snapshot.error);
        auto doc=std::move(prepared_snapshot.document);
        r.data["format"]=format; r.data["validation_level"]="preflight";
        IO::TiffExportOptions rip_options;
        rip_options.prevent_white_clipping=prevent_white_clipping;
        rip_options.include_transparent=prevent_white_clipping && white_clipping_transparent;
        rip_options.clean_edges=clean_edges || hard_edges;
        rip_options.hard_edges=hard_edges;
        if (request.command=="file.export" && format=="tiff")
            r.data["rip_options"]=object{{"prevent-white-clipping",rip_options.prevent_white_clipping},
                {"white-clipping-transparent",rip_options.include_transparent},
                {"clean-edges",rip_options.clean_edges},{"hard-edges",rip_options.hard_edges}};
        std::string payload;
        if (request.command=="file.save") {
            // Editable intake embeds all admitted image payloads before the live baseline.
            XML::rebase_hrefs(doc.get(),fs::u8path(path).parent_path().string().c_str(),false);
            doc->setDocumentFilename(path.c_str());
            payload=sp_repr_save_buf(doc->getReprDoc()).raw();
        } else {
            unsigned scopes=p.contains("page")+p.contains("ids")+p.contains("area")+p.contains("drawing");
            if (scopes!=1 || (p.contains("drawing") && !flag(p,"drawing"))) return reject(r,"invalid-target","Choose exactly one page, ids, area or drawing scope.");
            Geom::OptRect area;
            if (p.contains("page")) {
                auto index=value_to<unsigned>(p.at("page"));
                auto &manager=doc->getPageManager();
                if (manager.getPageCount()) { auto page=manager.getPage(index-1); if (page) area=page->getDocumentRect(); }
                else if (index==1) area=Geom::Rect::from_xywh(0,0,doc->getWidth().value("px"),doc->getHeight().value("px"));
            } else if (p.contains("area")) {
                auto const &a=p.at("area").as_object(); auto w=px(a.at("width")),h=px(a.at("height"));
                if (w>0 && h>0) area=Geom::Rect::from_xywh(px(a.at("x")),px(a.at("y")),w,h);
            } else if (p.contains("ids")) {
                std::vector<SPItem *> items; std::set<std::string> seen;
                for (auto const &id:p.at("ids").as_array()) {
                    std::string name(id.as_string()); auto item=cast<SPItem>(doc->getObjectById(name.c_str()));
                    if (!item || item==doc->getRoot() || !seen.insert(name).second) return reject(r,"invalid-target","IDs must name unique graphical objects.");
                    items.push_back(item); area.unionWith(item->documentVisualBounds());
                }
                // Native multi-object crop retains ancestors and referenced definitions.
                std::vector<SPObject *> objects(items.begin(),items.end());
                doc->getRoot()->cropToObjects(objects);
            } else area=doc->getRoot()->documentVisualBounds();
            if (!area || !std::isfinite(area->width()) || !std::isfinite(area->height()) || area->width()<=0 || area->height()<=0)
                return reject(r,"invalid-target","Scope has no finite positive bounds.");
            double dpi=number(p,"dpi",96); auto width=std::max(1.0,std::floor(area->width()*dpi/96+0.5)),height=std::max(1.0,std::floor(area->height()*dpi/96+0.5));
            constexpr std::uint64_t raster_pixel_limit = 103680000;
            if (width<1 || height<1 || width*height>raster_pixel_limit)
                return reject(r,"out-of-range","Raster export is limited to 103.68 MP (12 x 24 in at 600 dpi).");
            r.data["output_width"]=std::uint64_t(width); r.data["output_height"]=std::uint64_t(height);
            IO::PreparedExportProfile prepared; std::string error;
            bool raster=format=="png" || format=="tiff";
            if (!raster && p.contains("profile")) return reject(r,"profile-unsupported","SVG and PDF do not accept an output profile.");
            if (raster) {
                r.warnings.emplace_back("export-loss");
                ProfileError profile_error;
                if (!resolve_profile(p,grants,prepared,profile_error,hooks)) return reject(r,profile_error.code,profile_error.message);
                r.data["profile"]=object{{"id",str(p.contains("profile") ? p.at("profile").as_object() : object{},"id","default")},
                    {"name",prepared.profile.name},{"sha256",prepared.profile.sha256},{"notice",prepared.notice}};
                if (!prepared.notice.empty()) r.warnings.emplace_back("profile-fallback");
            }
            if (request.dry_run) {
                if (cancelled(context)) return reject(r,"cancelled","Cancelled during dry-run preparation.",Status::Cancelled);
                return r;
            }
            if (str(p,"text-policy","preserve")=="paths") convert_text_to_curves(doc.get());
            Scratch scratch;
            if (raster) {
                Colors::Color background(0x00000000);
                if (auto v=p.if_contains("background")) {
                    std::uint32_t rgba=0; for (auto const &c:v->as_array()) rgba=(rgba<<8)|std::uint32_t(std::round(value_to<double>(c)*255));
                    background=Colors::Color(rgba);
                }
                auto png=scratch.path("render.png");
                if (cli_fault("file.png.render", CliFaultKind::Renderer) || sp_export_png_file(doc.get(),png.c_str(),*area,width,height,dpi,dpi,background,nullptr,nullptr,true,{},false,6,8,6,2,
                    format=="png" ? &prepared : nullptr)!=EXPORT_OK) return reject(r,"export-failed","Native PNG rendering failed.",Status::Failed);
                if (format=="tiff") {
                    auto tiff=scratch.path("render.tiff");
                    if (!IO::export_png_to_color_managed_tiff(png,tiff,prepared.profile.bytes,error,nullptr,rip_options))
                        return reject(r,"export-failed",error,Status::Failed);
                    payload=read_payload(tiff);
                } else payload=read_payload(png);
            } else {
                if (auto bg=p.if_contains("background")) {
                    std::uint32_t rgba=0; for (auto const &c:bg->as_array()) rgba=(rgba<<8)|std::uint32_t(std::round(value_to<double>(c)*255));
                    if (rgba & 255) {
                        auto rect=doc->getReprDoc()->createElement("svg:rect");
                        auto local=*area * doc->getRoot()->c2p.inverse();
                        rect->setAttribute("x",std::to_string(local.left())); rect->setAttribute("y",std::to_string(local.top()));
                        rect->setAttribute("width",std::to_string(local.width())); rect->setAttribute("height",std::to_string(local.height()));
                        rect->setAttribute("fill",Colors::Color(rgba | 255).toString());
                        rect->setAttribute("fill-opacity",std::to_string(double(rgba & 255)/255));
                        doc->getReprRoot()->addChild(rect,nullptr); GC::release(rect);
                    }
                }
                doc->getPageManager().disablePages(); doc->fitToRect(*area); doc->ensureUpToDate();
                if (format=="svg") payload=sp_repr_save_buf(doc->getReprDoc()).raw();
                else if (format=="pdf") { auto pdf=scratch.path("render.pdf"); if (!render_pdf(doc.get(),pdf,dpi)) return reject(r,"export-failed","Native PDF rendering failed.",Status::Failed); payload=read_payload(pdf); }
                else return reject(r,"format-unsupported","Supported export formats are svg, png, pdf and tiff.");
            }
        }
        if (request.dry_run) {
            if (cancelled(context)) return reject(r,"cancelled","Cancelled during dry-run preparation.",Status::Cancelled);
            return r;
        }
        if (payload.size()>cli_counted_limit("file.payload.bytes", 512ull<<20)) return reject(r,"input-too-large","Serialized output exceeds the scratch limit.");
        // Re-admit immediately before E13; only E13 ever receives the real destination.
        if (inspect_write_destination(path,grants).state!="granted") return reject(r,"unsafe-destination","Destination changed before publication.");
        publication_started=true;
        return publish(r,request,context,state,payload,grants,hooks);
    } catch (std::exception const &e) {
        if (publication_started) {
            state.writes_blocked=true; state.reconciliation={{"destination",str(p,"path")},{"acknowledged",false}};
            r.publication="uncertain";
            return reject(r,"publication-uncertain",e.what(),Status::Uncertain);
        }
        return reject(r,"internal-error",e.what(),Status::Failed);
    }
}
Record execute_file(Request const &r, DispatchContext &c, FileState &s, Grants const &g) {
    return execute_file_impl(r,c,s,g,nullptr);
}
Record execute_file_for_testing(Request const &r, DispatchContext &c, FileState &s, Grants const &g,
                                FileServiceTestHooks const &hooks) {
    return execute_file_impl(r,c,s,g,&hooks);
}
boost::json::object file_capabilities()
{
    return {{"open",{"svg","svgz","cdr","pdf","png","jpeg","tiff"}},{"export",{"svg","png","pdf","tiff"}},
      {"profiles",{"default","srgb","file"}},{"missing_required_backends",array{}},
      {"accepted",false},{"expected_version",true},{"parent_race_safe",false}};
}
}
