// SPDX-License-Identifier: GPL-2.0-or-later
#include "nesting-cli-service.h"
#include "actions/actions-vacards-nest.h"
#include "actions/vacards-cli-fault.h"
#include <boost/json.hpp>
#include "document.h"
#include "selection.h"
#include "object/sp-item.h"
#include "util/operation-targets.h"
#include "xml/repr.h"
#include "xml/node.h"
#include <algorithm>
#include "xml/attribute-record.h"
#include <map>
#include <set>
#include <2geom/transforms.h>
namespace Inkscape::VACardsCli {
Nesting::SolveResult solve_nest_native(Nesting::PreparedDocumentNesting const &snapshot,
    Nesting::Options options, NestWorkOptions const &work,
    Nesting::Job::ProgressCallback const &progress, std::stop_token cancellation)
{
    if (work.workers != 1 || work.iterations.has_value() == work.timeout_ms.has_value() ||
        (work.iterations && !*work.iterations) || (work.timeout_ms && !*work.timeout_ms))
        return {.status = Nesting::Status::InvalidArgument, .error = "supply one positive work or time budget and one worker"};
    options.random_seed = work.seed;
    options.worker_count = 1;
    options.time_limit_ms = work.timeout_ms.value_or(0);
    return Nesting::solvePreparedNesting(snapshot, options, Nesting::EngineSelection::NativeOnly,
                                         work.iterations.value_or(0), progress, cancellation);
}

Record execute_nest(Request const &request, DispatchContext &dispatch, NestCliContext &context)
{
    if (request.command != "nest.contour-set" && request.command != "nest.contour-release")
        return production_unavailable(request); // Geometry/token routes require ProductionContext.
    auto &document = context.edits.document;
    Record result;
    result.action = request.command;
    result.dry_run = request.dry_run;
    result.mode = "collective-geometry";
    result.normalized_params = request.params;
    result.preferred_unit = dispatch.preferred_unit;
    auto stamp = document_stamp(&document);
    result.document_id = stamp.id;
    result.revision_before = result.revision_after = stamp.revision;
    for (auto *item : context.edits.selection.items())
        if (item->getId()) result.selection_after.emplace_back(item->getId());
    auto refuse = [&](std::string code, std::string reason, bool retryable = false) {
        result.status = code == "cancelled" ? Status::Cancelled : Status::Rejected;
        result.reason = code;
        result.message = reason;
        result.error = ParseError{code, {}, reason};
        result.error_details = {{"reason", reason}, {"mutation_state", "none"}};
        result.error_retryable = retryable;
        return result;
    };
    if (context.cancelled && context.cancelled())
        return refuse("cancelled", "cancellation observed before commit");
    auto resolve = [&](boost::json::value const &value) {
        return cast<SPItem>(document.getObjectById(std::string(value.as_string())));
    };
    Nesting::ContourBindingResult native;
    bool const setting = request.command == "nest.contour-set";
    if (setting) {
        auto *payload = resolve(request.params.at("payload-id"));
        auto *contour = resolve(request.params.at("contour-id"));
        if (!payload || !contour) return refuse("unknown-id", "explicit ID cannot resolve");
        native = Nesting::setNestingContour(document, payload, contour, request.dry_run, context.edits.operation_lease);
        result.selected = 2;
    } else {
        std::vector<SPItem *> roots;
        for (auto const &id : request.params.at("ids").as_array()) {
            auto *root = resolve(id);
            if (!root) return refuse("unknown-id", "explicit ID cannot resolve");
            roots.push_back(root);
        }
        result.selected = roots.size();
        auto resolved = Util::resolve_composite_targets(roots,
            [](SPItem *) { return Util::TargetAvailability::Eligible; },
            [](SPItem *item) { return cast<SPItem>(item->parent); },
            [](SPItem *) -> SPItem * { return nullptr; });
        result.covered = roots.size() - resolved.items.size();
        roots = std::move(resolved.items);
        native = Nesting::releaseNestingContour(document, roots, request.dry_run, context.edits.operation_lease);
        if (native.status == Nesting::ContourBindingStatus::Unchanged)
            return refuse("no-eligible-targets", "no bound root");
    }
    using Reason = Nesting::ContourBindingReason;
    if (native.status == Nesting::ContourBindingStatus::Refused) {
        if (native.reason == Reason::PublicationFailed) {
            auto failed = refuse("internal-error", "unexpected service exception; rollback before return");
            failed.status = Status::Failed;
            failed.error_details["mutation_state"] = "rolled-back";
            return failed;
        }
        if (native.reason == Reason::Busy) return refuse("transaction-unavailable", "EditTransaction inactive", true);
        if (native.reason == Reason::OverlappingRoles) return refuse("duplicate-id", "payload and contour roles equal");
        if (native.reason == Reason::InvalidItem) return refuse("unknown-id", "explicit ID cannot resolve");
        return refuse(setting ? "unsupported-target" : "unavailable", "binding geometry unsupported");
    }
    auto array = [](auto const &ids) {
        boost::json::array values;
        for (auto const &id : ids) values.emplace_back(id);
        return values;
    };
    result.data = {{"variant", request.dry_run ? "computed-dry-run" : "success"},
        {"binding-ids", array(native.binding_ids)}, {"payload-ids", array(native.payload_ids)},
        {"contour-ids", array(native.contour_ids)}};
    result.eligible = result.selected - result.covered;
    if (!request.dry_run) {
        result.status = Status::Changed;
        result.one_undo_step = true;
        result.undo_effect = "one-step";
        result.revision_after = document_stamp(&document).revision;
        result.modified = native.payload_ids;
        result.modified.insert(result.modified.end(), native.contour_ids.begin(), native.contour_ids.end());
        if (setting) {
            result.created = native.binding_ids;
            result.selection_after = native.binding_ids;
        }
    }
    return result;
}
namespace {
struct NestSourceCopy { std::string id; std::uint32_t copy; Geom::Affine original; };
struct NestAnalysis final : TokenPayload {
    TokenKind kind() const noexcept override { return TokenKind::NestAnalysis; }
    std::vector<NestSourceCopy> sources;
    Nesting::Options options;
    boost::json::object params, data;
    std::uint64_t bytes = 0;
};
struct NestSolution final : TokenPayload {
    TokenKind kind() const noexcept override { return TokenKind::NestSolution; }
    std::shared_ptr<NestAnalysis const> analysis;
    Nesting::SolveResult result;
};
std::string nest_hash(std::string const &bytes) {
    auto *sum=g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum,reinterpret_cast<guchar const *>(bytes.data()),bytes.size());
    std::string result=g_checksum_get_string(sum);g_checksum_free(sum);return result;
}
boost::json::value nest_xml(XML::Node const *node) {
    boost::json::object attrs;
    std::map<std::string,std::string> sorted;
    for(auto const &a:node->attributeList()) sorted[g_quark_to_string(a.key)]=static_cast<char const *>(a.value);
    for(auto const &[k,v]:sorted)attrs[k]=v;
    boost::json::array children;
    for(auto *child=node->firstChild();child;child=child->next())children.push_back(nest_xml(child));
    return boost::json::array{node->name()?node->name():"",std::move(attrs),node->content()?node->content():"",std::move(children)};
}
boost::json::array nest_ring(std::vector<Nesting::Point> const &ring) {
    boost::json::array points;for(auto const &p:ring)points.emplace_back(boost::json::array{p.x,p.y});return points;
}
boost::json::value nest_bounds(std::vector<Nesting::CollisionComponent> const &components) {
    Geom::OptRect bounds;
    for(auto const &c:components)for(auto const &p:c.outer)bounds.expandTo(Geom::Point(p.x,p.y));
    return bounds?boost::json::value(boost::json::array{bounds->left(),bounds->top(),bounds->right(),bounds->bottom()}):boost::json::value(nullptr);
}
boost::json::array nest_affine(Geom::Affine const &affine) {
    boost::json::array a;for(unsigned i=0;i<6;++i)a.emplace_back(affine[i]);return a;
}
std::string allocation_id() {auto *uuid=g_uuid_string_random();std::string id=uuid;g_free(uuid);return id;}
bool nest_bool(boost::json::object const &params,char const *name,bool fallback) {
    auto value=params.if_contains(name);return value?value->as_bool():fallback;
}
double nest_px(boost::json::object const &params,char const *name,double fallback=0) {
    auto value=params.if_contains(name);return value?value->as_object().at("value").to_number<double>():fallback;
}
struct NestPreparation {
    std::shared_ptr<Nesting::CliSession::Capture> capture;
    std::shared_ptr<NestAnalysis> analysis;
    std::string code, reason;
    std::vector<Exclusion> exclusions;
    std::size_t covered=0;
};
NestPreparation nest_prepare(SPDocument &doc,boost::json::object const &params, bool analyze = false) {
    NestPreparation out;
    auto fail=[&](std::string code,std::string reason){out.code=std::move(code);out.reason=std::move(reason);return out;};
    std::vector<SPItem *> roots,obstacles;
    for(auto const &id:params.at("ids").as_array()) {
        auto *item=cast<SPItem>(doc.getObjectById(std::string(id.as_string())));
        if(!item)return fail("unknown-id","explicit ID cannot resolve");roots.push_back(item);
    }
    auto resolved=Util::resolve_composite_targets(roots,[](SPItem *){return Util::TargetAvailability::Eligible;},
        [](SPItem *item){return cast<SPItem>(item->parent);},[](SPItem *)->SPItem *{return nullptr;});
    out.covered=roots.size()-resolved.items.size();roots=std::move(resolved.items);
    if(auto ids=params.if_contains("obstacles"))for(auto const &id:ids->as_array()) {
        auto *item=cast<SPItem>(doc.getObjectById(std::string(id.as_string())));
        if(!item)return fail("unknown-id","explicit ID cannot resolve");obstacles.push_back(item);
    }
    Nesting::RequestPreparationOptions options;
    if(auto page=params.if_contains("page"))options.page=page->to_number<unsigned>();
    if(auto sheet=params.if_contains("sheet")) {
        auto const &s=sheet->as_object();double x=nest_px(s,"x"),y=nest_px(s,"y");
        options.sheet=Geom::Rect(Geom::Point(x,y),Geom::Point(x+nest_px(s,"width"),y+nest_px(s,"height")));
    }
    options.solver_options.part_spacing=nest_px(params,"gap");options.solver_options.container_margin=nest_px(params,"margin");
    options.solver_options.rotation_mode=Nesting::RotationMode::None;
    if(auto rotation=params.if_contains("rotations")) {
        auto const &v=rotation->as_object();auto mode=v.at("mode").as_string();
        options.solver_options.rotation_mode=mode=="none"?Nesting::RotationMode::None:mode=="right-angles"?Nesting::RotationMode::RightAngles:mode=="discrete"?Nesting::RotationMode::Discrete:Nesting::RotationMode::Free;
        if(auto step=v.if_contains("step-degrees"))options.solver_options.rotation_step_degrees=step->to_number<double>();
    }
    options.reject_conservative=!params.if_contains("fallback")||params.at("fallback")=="reject";
    boost::json::array requested;
    std::size_t expanded=0;
    if(auto copies=params.if_contains("copies"))for(auto const &entry:copies->as_object()) {
        if(std::none_of(roots.begin(),roots.end(),[&](SPItem *item){return entry.key()==item->getId();}))
            return fail("invalid-input","capture/assemble: invalid page or copy keys");
    }
    for(auto *root:roots) {
        std::uint32_t count=1;
        if(auto copies=params.if_contains("copies"))if(auto v=copies->as_object().if_contains(root->getId()))count=v->to_number<std::uint32_t>();
        expanded+=count;options.copies.push_back(count);requested.emplace_back(boost::json::object{{"id",root->getId()},{"count",count}});
    }
    // The current native candidate-validation ABI has a hard 2048-part cap.
    // Refuse as an engine budget, never truncate a schema-valid copy request.
    if(expanded>2048)return fail("engine-limit","expanded copies >100000 or resource budget");
    auto native=Nesting::prepareRequestNesting(doc,roots,options,0.05,obstacles);
    if(!native) {
        for(auto const &skip:native.skipped_parts)if(skip.item && skip.item->getId())
            out.exclusions.push_back({skip.item->getId(),skip.detail});
        using R=Nesting::PreparationReason;
        if(analyze && native.reason==R::NoUsableParts)return fail("unsupported-target","capture: unusable part");
        if(native.reason==R::ConservativeRejected)return fail("unsupported-target","capture: conservative and fallback reject");
        if(native.reason==R::InvalidSheet||native.reason==R::InvalidCopies)return fail("invalid-input","capture/assemble: invalid page or copy keys");
        if(native.reason==R::ResourceLimit)return fail("engine-limit","expanded copies >100000 or resource budget");
        return fail("analysis-failed","prepare/assemble geometry failed");
    }
    out.capture=std::make_shared<Nesting::CliSession::Capture>();out.capture->snapshot=std::move(*native.snapshot);
    auto &snapshot=out.capture->snapshot;
    out.analysis=std::make_shared<NestAnalysis>();auto &analysis=*out.analysis;
    analysis.params=params;analysis.options=options.solver_options;
    boost::json::array recovery;
    analysis.bytes=sizeof(NestAnalysis)+sizeof(Nesting::CliSession::Capture);
    for(auto const &part:snapshot.parts) {
        auto id=std::string(part.item->getId());analysis.sources.push_back({id,part.copy,part.original_item_to_document});
        boost::json::array components;
        for(auto const &component:part.components) {
            boost::json::array holes;for(auto const &hole:component.holes){holes.push_back(nest_ring(hole));analysis.bytes+=hole.capacity()*sizeof(Nesting::Point);}
            components.emplace_back(boost::json::array{nest_ring(component.outer),std::move(holes)});
            analysis.bytes+=component.outer.capacity()*sizeof(Nesting::Point);
        }
        analysis.bytes+=sizeof(Nesting::PreparedPart)+sizeof(NestSourceCopy)+id.size();
        if(part.copy==0) {
            std::string mode=part.contour_source==Nesting::ContourSource::ConservativeHull?"conservative-hull":part.contour_source==Nesting::ContourSource::ConservativeBounds?"conservative-bounds":part.recovery==Nesting::RecoveryKind::Repaired?"repaired":"native";
            recovery.emplace_back(boost::json::object{{"id",id},{"mode",mode},{"bounds",nest_bounds(part.components)},
                {"source-sha256",nest_hash(boost::json::serialize(nest_xml(part.item->getRepr())))},
                {"collision-sha256",nest_hash(boost::json::serialize(components))}});
        }
    }
    for(auto const &obstacle:snapshot.obstacles)for(auto const &component:obstacle.components) {
        analysis.bytes+=component.outer.capacity()*sizeof(Nesting::Point);
        for(auto const &hole:component.holes)analysis.bytes+=hole.capacity()*sizeof(Nesting::Point);
    }
    for(auto const &skip:snapshot.skipped_parts)if(skip.item && skip.item->getId())out.exclusions.push_back({skip.item->getId(),skip.detail});
    std::vector<Nesting::CollisionComponent> sheet{{snapshot.container_outline,snapshot.container_holes}};
    analysis.data={{"analysis-token",nullptr},{"requested-copies",std::move(requested)},{"recovery",std::move(recovery)},
        {"resource-bytes",analysis.bytes},{"sheet-bounds",nest_bounds(sheet)}};
    analysis.bytes+=boost::json::serialize(analysis.data).size()+boost::json::serialize(analysis.params).size();
    analysis.data["resource-bytes"]=analysis.bytes;
    return out;
}
boost::json::object nest_solution_data(NestAnalysis const &analysis,Nesting::SolveResult const &solution) {
    boost::json::array placements,unplaced;
    for(std::size_t i=0;i<solution.placements.size();++i) {
        auto const &p=solution.placements[i];auto const &source=analysis.sources[i];
        if(!p.placed){unplaced.emplace_back(boost::json::object{{"id",source.id},{"copy",source.copy}});continue;}
        auto affine=source.original*Geom::Rotate::from_degrees(p.rotation_degrees)*Geom::Translate(p.translation_x,p.translation_y);
        placements.emplace_back(boost::json::object{{"id",source.id},{"copy",source.copy},{"affine",nest_affine(affine)}});
    }
    char const *stop="completed";
    if(solution.terminal->stop_reason==Nesting::StopReason::WorkLimit)stop="work-limit";
    if(solution.terminal->stop_reason==Nesting::StopReason::TimeLimit)stop="time-limit";
    return {{"solution-token",nullptr},{"engine",solution.backend},{"iterations",solution.terminal->completed_work},
        {"stop-reason",stop},{"deterministic",solution.deterministic},{"placements",std::move(placements)},
        {"unplaced",std::move(unplaced)},{"utilization",solution.metrics.utilization_percent}};
}
Record execute_nest_geometry(Request const &request,DispatchContext &dispatch,ProductionContext &context) {
    Record r;r.action=request.command;r.dry_run=request.dry_run;r.mode="collective-geometry";r.normalized_params=request.params;
    auto &doc=context.edits.document;auto stamp=document_stamp(&doc);r.document_id=stamp.id;r.revision_before=r.revision_after=stamp.revision;
    for(auto *item:context.edits.selection.items())if(item->getId())r.selection_after.emplace_back(item->getId());
    auto refuse=[&](std::string code,std::string reason,bool retry=false) {
        r.status=code=="cancelled"?Status::Cancelled:Status::Rejected;r.reason=code;r.message=reason;r.error=ParseError{code,{},reason};
        r.error_details={{"reason",reason},{"mutation_state","none"}};r.error_retryable=retry;r.data.clear();return r;
    };
    auto const tokens_before = context.tokens.snapshot();
    bool committed = false;
    try {
    auto cancelled=[&]{return dispatch.cancelled && dispatch.cancelled();};
    if(cancelled())return refuse("cancelled","cancellation observed before commit");
    if(!context.nest)return refuse("unavailable","nest owner capability unavailable");
    context.nest->prune(context.tokens.snapshot());
    TokenValidationContext validation{context.session_id,stamp.id,"native",context.catalog_identity,context.incarnation,stamp.revision,context.target_generation};
    auto binding=[&](boost::json::object const &params,NestAnalysis const &analysis) {
        TokenBinding b;b.session=context.session_id;b.document=stamp.id;b.engine="native";b.catalog_hash=context.catalog_identity;
        b.revision=stamp.revision;b.incarnation=context.incarnation;b.target_generation=context.target_generation;b.session_revision=dispatch.session_revision;
        b.normalized_params=boost::json::serialize(params);
        for(auto const &source:analysis.sources)if(source.copy==0)b.ordered_ids.push_back(source.id);
        for(auto const &entry:analysis.data.at("recovery").as_array()) {
            b.dependency_hashes.emplace_back(entry.as_object().at("source-sha256").as_string());
            b.dependency_hashes.emplace_back(entry.as_object().at("collision-sha256").as_string());
        }
        return b;
    };
    bool analyzing=request.command=="nest.analyze",solving=request.command=="nest.solve";
    std::shared_ptr<Nesting::CliSession::Capture> capture;
    std::shared_ptr<NestAnalysis const> analysis;
    std::optional<TokenLease> lease;
    std::shared_ptr<NestSolution const> retained_solution;
    if(analyzing || (!solving && !request.params.if_contains("solution-token"))) {
        auto prepared=nest_prepare(doc,analyzing?request.params:request.params.at("analyze").as_object(),analyzing);
        if(!prepared.capture) {r.excluded=prepared.exclusions;return refuse(prepared.code,prepared.reason);}
        capture=prepared.capture;analysis=prepared.analysis;r.excluded=prepared.exclusions;r.covered=prepared.covered;
        r.selected=(analyzing?request.params:request.params.at("analyze").as_object()).at("ids").as_array().size();
        r.eligible=std::count_if(analysis->sources.begin(),analysis->sources.end(),[](auto const &source){return source.copy==0;});
    } else {
        auto token=std::string(request.params.at(solving?"analysis-token":"solution-token").as_string());
        auto found=context.tokens.lookup(token,solving?TokenKind::NestAnalysis:TokenKind::NestSolution,validation);
        if(!found.value)return refuse(found.error->code,found.error->message,found.error->retryable);
        lease=std::move(found.value);
        std::string root=solving?lease->id:lease->parent->id;
        capture=context.nest->lookup(root);
        if(!capture || !Nesting::preparedNestingFresh(capture->snapshot))return refuse("stale-dependency","native freshness check");
        if(solving)analysis=std::dynamic_pointer_cast<NestAnalysis const>(lease->payload);
        else {retained_solution=std::dynamic_pointer_cast<NestSolution const>(lease->payload);if(retained_solution)analysis=retained_solution->analysis;}
        if(!analysis)return refuse("invalid-token","token payload kind mismatch");
    }
    cli_fault_throw(request.command + ".after-prepare");
    if (!analyzing && r.selected == 0) {
        r.selected=analysis->data.at("requested-copies").as_array().size();
        r.eligible=std::count_if(analysis->sources.begin(),analysis->sources.end(),[](auto const &source){return source.copy==0;});
    }
    for (auto const &entry:analysis->data.at("recovery").as_array()) {
        auto const &mode=entry.as_object().at("mode");
        std::string warning=mode=="repaired"?"geometry-repaired":mode=="native"?"":"conservative-recovery";
        if(!warning.empty() && std::find(r.warnings.begin(),r.warnings.end(),warning)==r.warnings.end())r.warnings.push_back(warning);
    }
    if(cancelled())return refuse("cancelled","cancellation observed before commit");
    if(analyzing) {
        r.data=analysis->data;
        if(!request.dry_run && nest_bool(request.params,"retain",true)) {
            if(context.session_id.empty())return refuse("session-required","retention requires a session");
            std::vector<TokenAllocation> allocations{{allocation_id(),analysis->bytes,capture}};
            auto stored=context.tokens.retain(TokenKind::NestAnalysis,binding(request.params,*analysis),analysis,std::move(allocations));
            if(!stored.value)return refuse(stored.error->code,stored.error->message,stored.error->retryable);
            cli_fault_throw("nest.analyze.after-token-retain");
            context.nest->retain(stored.value->id,capture);r.data["analysis-token"]=stored.value->id;
        }
        r.data["variant"]=request.dry_run?"computed-dry-run":"success";return r;
    }
    Nesting::SolveResult solution;
    if(retained_solution)solution=retained_solution->result;
    else {
        auto const &params=solving?request.params:request.params.at("solve").as_object();
        NestWorkOptions work;
        if(auto seed=params.if_contains("seed"))work.seed=seed->to_number<std::uint64_t>();
        if(auto iterations=params.if_contains("iterations"))work.iterations=iterations->to_number<std::uint64_t>();
        if(auto timeout=params.if_contains("timeout-ms"))work.timeout_ms=timeout->to_number<std::uint64_t>();
        std::stop_source stop;
        solution=solve_nest_native(capture->snapshot,analysis->options,work,[&](auto const &){if(cancelled())stop.request_stop();},stop.get_token());
    }
    if(solution.status==Nesting::Status::Cancelled || cancelled() || (solution.terminal && solution.terminal->stop_reason==Nesting::StopReason::Cancelled)) {
        auto failed=refuse("cancelled","native cancelled");failed.error_details["stop-reason"]="cancelled";return failed;
    }
    if(!solution || !solution.terminal)return refuse("solver-failed","native worker failed");
    auto data=nest_solution_data(*analysis,solution);
    if(solving) {
        if(!request.dry_run && nest_bool(request.params,"retain",true)) {
            if(context.session_id.empty())return refuse("session-required","retention requires a session");
            auto payload=std::make_shared<NestSolution>();payload->analysis=analysis;payload->result=solution;
            auto allocations=lease->allocations;
            allocations.push_back({allocation_id(),sizeof(NestSolution)+solution.placements.capacity()*sizeof(Nesting::Placement),payload});
            auto stored=context.tokens.retain(TokenKind::NestSolution,binding(request.params,*analysis),payload,std::move(allocations),TokenParent{lease->id,TokenKind::NestAnalysis});
            if(!stored.value)return refuse(stored.error->code,stored.error->message,stored.error->retryable);
            cli_fault_throw("nest.solve.after-token-retain");
            data["solution-token"]=stored.value->id;
        }
        data["variant"]=request.dry_run?"computed-dry-run":"success";r.data=std::move(data);return r;
    }
    if(!data.at("unplaced").as_array().empty() && (!request.params.if_contains("partial")||request.params.at("partial")=="reject"))
        return refuse("partial-result","unplaced copies and partial reject");
    std::unique_ptr<SPDocument> preview;
    if(request.dry_run) {
        auto xml=sp_repr_save_buf(doc.getReprDoc()).raw();preview=SPDocument::createNewDocFromMem(std::span<char const>(xml.data(),xml.size()));
        if(!preview)return refuse("internal-error","could not create preview document");preview->ensureUpToDate();
        auto projected=nest_prepare(*preview,analysis->params);
        if(!projected.capture)return refuse(projected.code,projected.reason);capture=projected.capture;
    }
    if(cancelled())return refuse("cancelled","cancellation observed before commit");
    cli_fault_throw("nest.apply.before-native-apply");
    auto applied=Nesting::applyNestingPlacements(capture->snapshot,solution.placements,{},request.dry_run?std::shared_ptr<void>{}:context.edits.operation_lease);
    if(applied.status==Nesting::ApplyStatus::StaleSnapshot)return refuse("stale-dependency","native freshness check");
    if(applied.status==Nesting::ApplyStatus::UndoUnavailable)return refuse("transaction-unavailable","EditTransaction inactive",true);
    if(applied.status==Nesting::ApplyStatus::PublicationFailed) {
        auto failed=refuse("publication-failed","copy insertion/apply failed; native rollback");
        failed.status=Status::Failed;failed.error_details["mutation_state"]="rolled-back";return failed;
    }
    if(applied.status==Nesting::ApplyStatus::InvalidResult)return refuse("invalid-placement","Job::validate: overlap/outside/invalid pose");
    committed = !request.dry_run && applied.changed();
    std::map<std::uint64_t,std::string> created(applied.created_copies.begin(),applied.created_copies.end());
    boost::json::array outputs;
    for(std::size_t i=0;i<solution.placements.size();++i) {
        auto const &p=solution.placements[i];if(!p.placed)continue;auto const &source=analysis->sources[i];
        auto output=source.copy?created.at(p.part_id):source.id;
        auto affine=source.original*Geom::Rotate::from_degrees(p.rotation_degrees)*Geom::Translate(p.translation_x,p.translation_y);
        outputs.emplace_back(boost::json::object{{"source",source.id},{"copy",source.copy},{"output-id",request.dry_run && source.copy?boost::json::value(nullptr):boost::json::value(output)},{"affine",nest_affine(affine)}});
        if(!request.dry_run && applied.changed()) {
            if(source.copy)r.created.push_back(output);else r.modified.push_back(output);
        }
    }
    r.data={{"variant",request.dry_run?"computed-dry-run":applied.changed()?"success":"unchanged"},
        {"source-copy",std::move(outputs)},{"placements",data.at("placements")},{"unplaced",data.at("unplaced")}};
    if(!request.dry_run) {
        r.status=applied.changed()?Status::Changed:Status::Unchanged;
        if(applied.changed()) {
            r.one_undo_step=true;r.undo_effect="one-step";r.revision_after=document_stamp(&doc).revision;
            if(lease)context.tokens.consume_after_commit(lease->id,validation);
            context.nest->prune(context.tokens.snapshot());
        }
    }
    return r;
    } catch (...) {
        // Native apply owns XML/history rollback. Never claim rollback after its commit.
        if (committed) throw;
        auto current = context.tokens.snapshot();
        for (auto const &id : current.active_tokens)
            if (std::find(tokens_before.active_tokens.begin(), tokens_before.active_tokens.end(), id) == tokens_before.active_tokens.end())
                context.tokens.release_subtree(id);
        if (context.nest) context.nest->prune(context.tokens.snapshot());
        auto failed = refuse("internal-error", "unexpected service exception; rollback before return");
        failed.status = Status::Failed;
        return failed;
    }
}
}

Record execute_nest(Request const &request, DispatchContext &dispatch, ProductionContext &context)
{
    if(request.command=="nest.contour-set"||request.command=="nest.contour-release") {
        NestCliContext native{context.edits, context.tokens, dispatch.cancelled};
        return execute_nest(request, dispatch, native);
    }
    return execute_nest_geometry(request,dispatch,context);
}

}

namespace Inkscape::Nesting {
struct CliSession::Impl { std::map<std::string,std::shared_ptr<Capture>> captures; };
CliSession::CliSession():_impl(std::make_unique<Impl>()) {}
CliSession::~CliSession() { retire(); }
namespace {
void retire_capture(CliSession::Capture &capture) noexcept {
    auto &s=capture.snapshot;s.document=nullptr;s.container=SPWeakPtr<SPItem>();s.revision.reset();
    for(auto &part:s.parts){part.item=SPWeakPtr<SPItem>();part.image_alpha_hashes.clear();}
    for(auto &obstacle:s.obstacles){obstacle.item=SPWeakPtr<SPItem>();obstacle.image_alpha_hashes.clear();}
    s.skipped_parts.clear();
}
}
void CliSession::retire() noexcept {for(auto &[id,capture]:_impl->captures)retire_capture(*capture);_impl->captures.clear();}
void CliSession::prune(VACardsCli::TokenSnapshot const &tokens) {
    std::set<std::string> active(tokens.active_tokens.begin(),tokens.active_tokens.end());
    for(auto it=_impl->captures.begin();it!=_impl->captures.end();) {
        if(!active.contains(it->first)){retire_capture(*it->second);it=_impl->captures.erase(it);}else ++it;
    }
}
void CliSession::retain(std::string const &id,std::shared_ptr<Capture> capture) {_impl->captures.emplace(id,std::move(capture));}
std::shared_ptr<CliSession::Capture> CliSession::lookup(std::string const &id) const {
    auto found=_impl->captures.find(id);return found==_impl->captures.end()?nullptr:found->second;
}
}
