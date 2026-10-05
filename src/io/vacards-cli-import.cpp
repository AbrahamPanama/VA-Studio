// SPDX-License-Identifier: GPL-2.0-or-later
#include "io/vacards-cli-import.h"
#include "io/vacards-cli-intake.h"
#include "object/sp-item.h"
#include "selection.h"
#include "util/units.h"
#include "xml/repr.h"
#include <boost/json.hpp>
namespace Inkscape::VACardsCli {
Record import_document(Request const &request, DispatchContext &context, Grants const &grants, IntakeObserver const &observer)
{
    using namespace boost::json;
    Record r; r.action=request.command; r.dry_run=request.dry_run;
    auto reject = [&](std::string code, std::string message) {
        r.created.clear(); r.modified.clear(); r.deleted.clear(); r.status=Status::Rejected; r.reason=code; r.message=message; r.error=ParseError{code,{},message}; return r;
    };
    if (!context.document) return reject("no-document","Import requires an editable document.");
    auto stamp=document_stamp(context.document); r.document_id=stamp.id; r.revision_before=r.revision_after=stamp.revision;
    auto str=[&](char const *k, char const *fallback="") { auto v=request.params.if_contains(k); return v ? std::string(v->as_string()) : fallback; };
    FileLoadOptions options{str("format","auto"),str("resource-policy"),str("font-policy"),{}};
    options.observe_for_testing = observer;
    if (auto p=request.params.if_contains("pages")) for (auto const &v:p->as_array()) options.pages.push_back(value_to<unsigned>(v));
    auto loaded=load_editable_document(str("path"),grants,options);
    if (loaded.error) {
        r.error_details = loaded.error->details;
        r.error_retryable = loaded.error->retryable;
        return reject(loaded.error->code,loaded.error->message);
    }
    if (auto fonts=loaded.report.if_contains("fonts"); fonts && !fonts->as_array().empty()) r.warnings.emplace_back("font-substitution");
    bool has_items=false;
    for (auto node=loaded.document->getReprRoot()->firstChild();node;node=node->next())
        if (cast<SPItem>(loaded.document->getObjectByRepr(node))) { has_items=true; break; }
    if (!has_items) return reject("import-failed","The source contains no importable graphical objects.");
    auto copy=request.dry_run ? context.document->copy() : nullptr;
    auto doc=copy ? copy.get() : context.document;
    auto selection=copy ? copy->getSelection() : context.selection;
    auto const &pos=request.params.at("position").as_object();
    auto px=[](value const &v) { auto const &l=v.as_object(); return Util::Quantity::convert(value_to<double>(l.at("value")),std::string(l.at("unit").as_string()),"px"); };
    double x=px(pos.at("x")), y=px(pos.at("y"));
    try {
        EditTransaction transaction(doc,selection,copy ? std::shared_ptr<void>{} : context.operation_lease);
        if (!transaction.active()) return reject("document-busy","Cannot acquire native import transaction.");
        std::vector<XML::Node *> inserted;
        doc->import(*loaded.document,nullptr,nullptr,Geom::Translate(x,y),&inserted,
                    SPDocument::ImportRoot::AlwaysGroup,SPDocument::ImportLayersMode::None,
                    SPDocument::ImportResources::Independent);
        doc->ensureUpToDate();
        array ids, affines;
        for (auto node:inserted) if (auto item=cast<SPItem>(doc->getObjectByRepr(node))) {
            if (item->getId()) {
                ids.emplace_back(item->getId()); r.created.emplace_back(item->getId());
                array affine; auto a=item->i2doc_affine(); for (unsigned i=0;i<6;++i) affine.emplace_back(a[i]);
                affines.emplace_back(object{{"id",item->getId()},{"affine",affine}});
            }
        }
        if (ids.empty()) return reject("import-failed","The source produced no importable roots.");
        if (context.cancelled && context.cancelled()) { r.status=Status::Cancelled; r.reason="cancelled"; return r; }
        r.data={{"inserted_ids",ids},{"root_affines",affines},{"exclusions",array{}},{"intake",loaded.report},
                {"validation_level","computed"}};
        if (serialize(r.data).size()>(8u<<20)-65536) return reject("engine-limit","Import report exceeds response limit.");
        if (request.dry_run) { transaction.rollback(); r.status=Status::Ok; r.created.clear(); }
        else {
            transaction.commit(Inkscape::Util::Internal::ContextString("Import document"), "document-import");
            r.status=Status::Changed; r.one_undo_step=true; r.undo_effect="one-step";
            r.revision_after=document_stamp(doc).revision;
        }
    } catch (std::exception const &e) { return reject("import-failed",e.what()); }
    return r;
}
}
