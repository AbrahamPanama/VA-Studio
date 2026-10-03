// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_TOOLS_CORNER_ROUNDING_CONTROLLER_H
#define INKSCAPE_UI_TOOLS_CORNER_ROUNDING_CONTROLLER_H
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <sigc++/signal.h>
#include "corner-rounding-context.h"
class SPDesktop;
class SPDocument;
class SPShape;
class LivePathEffectObject;
namespace Inkscape::UI::Tools {
// No XML preview: inspect/open/cancel are read-only; apply is one synchronous
// rollbackable native LPE edit. Registered document owners are leased during
// mutation; a private owner must retain its document through each call.
class CornerRoundingController {
public:
    using Request = LivePathEffect::CornerEdit::Request;
    struct View {
        CornerRoundingContext context;
        LivePathEffect::CornerEdit::Summary summary;
        std::uint64_t generation = 0;
    };
    enum class Outcome { Applied, NoChange, Rejected, Superseded };
    struct Result { Outcome outcome; std::string reason; };
    explicit CornerRoundingController(SPDesktop &);
    ~CornerRoundingController();
    View inspect(LivePathEffect::CornerEdit::Scope,
                 std::optional<LivePathEffect::CornerEdit::Address> clicked_corner = {});
    Result apply(Request const &, std::uint64_t generation);
    void invalidate();
    sigc::connection connectInvalidated(sigc::slot<void()>);
private:
    struct State;
    std::shared_ptr<State> _state;
};

// Who owns the Undo protocol for a corner commit. The caller decides and the
// shared implementation never guesses:
//   Interaction -- the Node-tool popover. A rollbackable interaction token is
//     held for the whole mutation (so it refuses, with nothing changed, while
//     another document interaction is active) and the same reject texts and
//     rollback/Superseded handling as the live tool are used.
//   CommandLine -- a VACards action. No interaction token is taken; the commit
//     refuses before any change while the document is closing or another
//     document interaction is active, then performs the identical XML changes
//     and records exactly one Undo step (or DocumentUndo::cancel on a failure
//     after the first mutation).
enum class CornerCommitProtocol { Interaction, CommandLine };

struct CornerApplyResult {
    CornerRoundingController::Outcome outcome = CornerRoundingController::Outcome::Rejected;
    std::string reason;
    bool mutation_started = false;
    bool refused_by_document = false; ///< CommandLine admission refused (busy, closing, no Undo)
    LivePathEffectObject *effect = nullptr;
    std::size_t count = 0;
    bool approximate = false;
};

// The read-only part of apply_corner_plan: the plan and every refusal/no-change
// check, with identical outcomes and messages. `ready` is true exactly when
// apply_corner_plan would go on to mutate the document (used by dry runs).
struct CornerPlanCheck {
    bool ready = false;
    CornerRoundingController::Outcome outcome = CornerRoundingController::Outcome::Rejected;
    std::string reason;
    LivePathEffect::CornerEdit::Plan plan;
};
CornerPlanCheck check_corner_plan(bool has_effect, LivePathEffect::CornerEdit::Snapshot const &fresh,
                                  LivePathEffect::CornerEdit::Request const &request);

// CommandLine admission shared by the commit and dry runs: the reason the
// document cannot take a command-line corner edit now (closing, another
// interaction active, Undo recording off), or nothing when it can.
std::optional<std::string> command_line_corner_refusal(SPDocument &document);

// The shared corner commit: plan, persist and record one corner edit on
// @a target. @a effect is the target's existing private Corners effect (or null
// for a fresh one); the result carries the possibly-newly-created effect so the
// Node-tool controller can store it. @a fresh is the validated effect-input
// snapshot. @a still_current is polled before the mutation and before publish;
// it must be false once the interaction is superseded. @a before_mutation runs
// exactly once, immediately before the first document mutation and only when the
// protocol admitted the commit; it is never called on a refusal. `count` and
// `approximate` come from the plan. GUI outcomes and messages are identical to
// the pre-shared implementation.
CornerApplyResult apply_corner_plan(SPShape &target, LivePathEffectObject *effect,
                                    LivePathEffect::CornerEdit::Snapshot const &fresh,
                                    LivePathEffect::CornerEdit::Request const &request,
                                    CornerCommitProtocol protocol,
                                    std::function<bool()> const &still_current,
                                    std::function<void()> const &before_mutation = {});
}
#endif
