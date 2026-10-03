// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_UI_STROKE_WIDTH_CONTROLLER_H
#define INKSCAPE_UI_STROKE_WIDTH_CONTROLLER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <sigc++/connection.h>

#include "document-undo.h"
#include "xml/node-observer.h"
#include "object/weakptr.h"

class SPDocument;
class SPItem;
class SPObject;
namespace Inkscape::Util { class Unit; }

namespace Inkscape::UI {

/**
 * Read-only stroke-width target resolution and query.
 *
 * SW1 only observes an already-current document. It never rebuilds layout,
 * mutates XML, selection, defaults, Undo/Redo or any transaction. Callers own
 * currency (SPDocument::ensureUpToDate) and any later write/transaction work.
 */

/// Availability of a resolved node for a stroke-width edit.
enum class StrokeWidthEligibility {
    Eligible,      ///< independent compatible target/run
    Incompatible,  ///< wrong kind of object (bitmap, unsupported container)
    Unavailable,   ///< right kind but protected/unsafe/unverified
    Covered,       ///< already produced once through an ancestor/duplicate root
    MissingSource, ///< clone/use whose referenced source could not be resolved
};

/// Concrete reason behind a non-eligible outcome or an excluded run.
enum class StrokeWidthExclusion {
    None,
    Bitmap,
    UnsupportedType,
    UnsupportedContainer,
    TextDescendant,
    HiddenAncestor,
    LockedAncestor,
    DefinitionOrReference,
    NonFiniteTransform,
    SingularTransform,
    CloneSourceOverrides,
    CloneUnsupportedChild,
    MissingSource,
    UnsupportedTref,
    InvalidTextRange,
    TextRangeOutOfBounds,
    EmptySelection,
    NotBoundToRoot,
    WrongDocument,
    InvalidStyle,
    EmptyText,
};

enum class StrokeWidthTargetKind {
    Shape,
    TextOwner,
    CloneInstance,
    Container,
    Other,
};

/// How the effective width is defined by the renderer.
enum class StrokeWidthConvention {
    Ordinary,   ///< effective document px = local * i2doc descrim
    NonScaling, ///< vector-effect:non-scaling-stroke; effective px is
                ///< local_computed for every scope (shape, clone and text)
    Hairline,   ///< -inkscape-stroke:hairline (no numeric value)
};

enum class StrokeWidthQuery { Empty, Uniform, Mixed };

/// Per-run/target effective style snapshot. Read-only values only.
struct StrokeWidthStyle {
    /// SPStyle::stroke_width.computed in the owning item's user units.
    double local_computed = 0.0;
    /// Document CSS px for Ordinary; for NonScaling it is local_computed for
    /// every scope. Unset only for Hairline.
    std::optional<double> effective_px;
    StrokeWidthConvention convention = StrokeWidthConvention::Ordinary;
    bool paint_none = false;
    bool paint_important = false;
    unsigned char paint_style_src = 0;
    bool hairline_important = false;
    unsigned char hairline_style_src = 0;
    bool dash_set = false;
    std::vector<double> dash_computed;
    double dash_offset_computed = 0.0;
    bool width_set = false;
    bool width_inherit = false;
    bool width_important = false;
    /// Raw SPStyleSrc ordinals for stroke-width, stroke-dasharray and
    /// stroke-dashoffset (0=UNSET, 1=ATTRIBUTE, 2=STYLE_PROP, 3=STYLE_SHEET).
    /// Kept as small integers so this header stays free of style-internal.h;
    /// read-only input for the later per-property write-priority decision.
    /// stroke-dasharray and stroke-dashoffset carry independent source and
    /// !important flags and must never share a priority decision.
    unsigned char width_style_src = 0;
    unsigned char dash_style_src = 0;
    unsigned char dashoffset_style_src = 0;
    bool dasharray_important = false;
    bool dashoffset_important = false;
    /// Raw vector-effect:non-scaling-stroke flag. Stays true even when a native
    /// hairline extension also wins the convention; see Result::non_scaling.
    bool non_scaling = false;
    /// Exact native SPStyle::vector_effect value (for example "none",
    /// "non-scaling-stroke" or "non-scaling-stroke fixed-position") and its
    /// independent source/priority. Read-only metadata: `non_scaling` above
    /// stays the settled convention input. Kept as a plain string so this header
    /// stays free of style-internal.h; the value is the native parser's, never
    /// an authored SPString.
    std::string vector_effect_value;
    bool vector_effect_set = false;
    bool vector_effect_inherit = false;
    bool vector_effect_important = false;
    /// Raw SPStyleSrc ordinal, matching the width/dash style_src convention.
    unsigned char vector_effect_style_src = 0;
};

/// One contiguous logical style run. Range is half-open and only meaningful
/// for TextOwner targets.
struct StrokeWidthRun {
    unsigned first_char = 0;
    unsigned last_char = 0;
    SPWeakPtr<SPObject> style_source;
    StrokeWidthStyle style;
    StrokeWidthEligibility eligibility = StrokeWidthEligibility::Eligible;
    StrokeWidthExclusion exclusion = StrokeWidthExclusion::None;
};

/// One frozen whole-owner logical character's rendered relevant-stroke snapshot,
/// captured read-only at query time from the character's actual rendering style
/// source. Plain values only: no layout iterator, SPStyle pointer or retained
/// reference. `glyph` records whether the logical character is a real glyph; a
/// line-break control has no rendered stroke to compare.
struct StrokeWidthCharBaseline {
    bool glyph = false;
    bool authored = false; ///< layout TEXT_SOURCE, independent of glyph presence
    int glyph_index = -1;
    std::array<double, 2> anchor{};
    StrokeWidthStyle style;
};

struct StrokeWidthSourceBaseline {
    SPWeakPtr<SPObject> source;
    std::optional<std::string> inline_style;
    std::vector<std::string> computed_style; ///< lossless values in SPStyle property order
    std::map<std::string, std::string> inline_declarations;
    std::vector<std::string> changed_properties; ///< exact keys emitted by the frozen patch
    std::vector<bool> computed_patched; ///< property-order whitelist, never extended properties
    /// Partial owners: exact positioning, including absence, on retained authored tspans.
    std::optional<std::array<std::optional<std::string>, 5>> positions;
};

struct StrokeWidthTarget {
    SPWeakPtr<SPItem> owner;
    StrokeWidthTargetKind kind = StrokeWidthTargetKind::Shape;
    bool whole_object = true;
    /// True only for a caret scope; raw_first_char/raw_last_char then hold the
    /// caller's collapsed caret index instead of the normalized [0,count) range.
    bool caret_scope = false;
    bool empty_text = false;
    /// Normalized half-open logical range used for traversal.
    unsigned first_char = 0;
    unsigned last_char = 0;
    /// Raw caller metadata, preserved without reordering.
    unsigned raw_first_char = 0;
    unsigned raw_last_char = 0;
    bool reversed = false;
    StrokeWidthEligibility eligibility = StrokeWidthEligibility::Eligible;
    StrokeWidthExclusion exclusion = StrokeWidthExclusion::None;
    bool safe_transform = true;
    /// i2doc_affine().descrim() captured at query time.
    double transform_scale = 1.0;
    /// Referenced original for CloneInstance; weak, never dereferenced for write.
    SPWeakPtr<SPItem> clone_source;
    /// Whole-owner semantic text frozen at query time as the native text-only
    /// multiline string (`sp_te_get_string_multiline`). For a partial range this
    /// is still the entire owner's text; it is the content a later writer must
    /// compare, deliberately independent of XML span structure that native
    /// styling may split/normalize. Empty for non-text targets.
    std::string text_content;
    /// True when the owner had a native layout at query time. Explicit presence
    /// keeps a missing layout from comparing equal to a valid empty owner (both
    /// would otherwise carry an empty string and count 0). False for non-text.
    bool text_content_available = false;
    /// Whole-owner native logical character count:
    /// layout->iteratorToCharIndex(layout->end()). 0 when no layout exists.
    unsigned char_count = 0;
    std::vector<StrokeWidthRun> runs;
    /// Whole-owner per-logical-character rendered stroke baseline frozen at
    /// query time, indexed by logical character. It covers every index,
    /// including characters outside an explicit partial range, so a later write
    /// can prove the unselected region unchanged. Each entry is captured through
    /// the character's actual rendering style source
    /// (`nearest_style_source(raw_source, owner)`), never
    /// `sp_te_style_at_position`, so a non-inheriting vector-effect value and its
    /// independent !important priority survive native span splitting. Size
    /// equals `char_count` whenever a layout existed (`text_content_available`);
    /// empty for non-text targets and for a missing layout. A character whose
    /// rendering source is unavailable excludes the whole text target rather than
    /// inventing a style. Used read-only by preflight, the postwrite verifier and
    /// commit-readiness; never a write instruction.
    std::vector<StrokeWidthCharBaseline> char_baseline;
    /// Exact glyph/font/cluster/transform/visibility signature; no layout references.
    std::vector<std::pair<std::string, std::array<double, 12>>> glyph_signature;
    std::vector<StrokeWidthSourceBaseline> source_baseline;
    std::vector<std::string> authored_tree;
};

/// Explicit logical text scope supplied by the entry point.
struct StrokeWidthTextRange {
    SPWeakPtr<SPItem> owner; ///< must resolve to SPText/SPFlowtext
    /// true -> whole owner [0,count). first_char/last_char must be collapsed and
    /// not exceed count; otherwise the query rejects, never broadening a stale
    /// caret. The raw collapsed index is preserved on the target metadata.
    bool caret = false;
    unsigned first_char = 0; ///< raw caller start; may exceed last for reverse
    unsigned last_char = 0;  ///< raw caller end; may precede first
};

struct StrokeWidthResult {
    StrokeWidthQuery state = StrokeWidthQuery::Empty;
    bool range_rejected = false;
    StrokeWidthExclusion range_exclusion = StrokeWidthExclusion::None;
    /// Counting rule: each selected root and each visited member has one role.
    /// Ordinary group wrappers are traversal-only and add no numeric count;
    /// eligible shapes/text owners count as eligible; bitmaps as incompatible;
    /// protected objects as unavailable. A valid range owner inside a selected
    /// group counts once as eligible or unavailable (its range-scoped record),
    /// never again as a whole-owner record. A stale/invalid range rejects before
    /// traversal and has no target count. Repeated eligible non-range roots add
    /// covered records, which are distinct from writable targets. The first
    /// five numeric categories are disjoint target records.
    std::size_t eligible = 0;
    std::size_t incompatible = 0;
    std::size_t unavailable = 0;
    std::size_t covered = 0;
    std::size_t missing_sources = 0;
    /// Orthogonal target-level flag counts over eligible runs only; any eligible
    /// matching run contributes once per owner. Excluded targets and runs retain
    /// their raw style flags in their snapshot records but are not counted here.
    std::size_t paint_none = 0;
    std::size_t hairline = 0;
    /// Ordinary NonScaling convention only. A hairline fallback that also carries
    /// vector-effect is counted as hairline, never as ordinary non-scaling.
    std::size_t non_scaling = 0;
    /// Set only for Uniform ordinary results; never an average.
    std::optional<double> uniform_px;
    std::vector<StrokeWidthTarget> targets;  ///< eligible targets only
    std::vector<StrokeWidthTarget> excluded; ///< reasoned non-eligible records
};

/// Query all independent roots and eligible ordinary `svg:g` descendants.
StrokeWidthResult query_stroke_widths(SPDocument &document,
                                      std::vector<SPItem *> const &roots);

/// Query an explicit logical text scope. A valid range eclipses `roots`; an
/// invalid range rejects the whole query and never falls back to `roots`.
StrokeWidthResult query_stroke_widths(SPDocument &document,
                                      StrokeWidthTextRange const &text_range,
                                      std::vector<SPItem *> const &roots);

// ---------------------------------------------------------------------------
// Read-only width preparation (SW2A). No XML, SPCSSAttr, default, layout or
// history write happens here; these APIs only snapshot intended numeric shape
// patch values for a later caller-owned write phase.
// ---------------------------------------------------------------------------

/// Explicit width operation intent. The caller always chooses the mode/unit;
/// the controller never infers one and numeric zero is never reinterpreted as
/// RemoveStroke. Absolute values are already normalized to document CSS px by
/// the existing native unit conversion at the entry point.
enum class StrokeWidthIntentKind {
    AbsoluteCssPx,
    RelativePercent,
    Hairline,
    RemoveStroke,
    AdditiveCssPx,
};

struct StrokeWidthIntent {
    StrokeWidthIntentKind kind = StrokeWidthIntentKind::AbsoluteCssPx;
    /// CSS px for AbsoluteCssPx, percent for RelativePercent (200 => ×2),
    /// signed document CSS px delta for AdditiveCssPx (finite, nonzero, |v| <= 1e6).
    double value = 0.0;
    /// Honour /options/dash/scale: when true, a width change also scales each
    /// member's own authored dash pattern/offset by new_local / old_local.
    bool scale_dashes = false;
};

/// Plan-level state. `Rejected` means nothing was planned; partial known
/// exclusions keep `Prepared` and are reported per member / in `query`.
enum class StrokeWidthPlanState { Prepared, Rejected };

enum class StrokeWidthMemberOutcome { Change, Unchanged, Excluded };

/// Write-specific reasons, distinct from StrokeWidthExclusion query reasons.
/// StalePlan/InvalidTransaction/PostconditionMismatch are phase-B declarations;
/// this phase never returns them.
enum class StrokeWidthMemberReason {
    None,
    InvalidIntent,
    UnsupportedIntent,
    TextAdapterPending,
    CloneAdapterPending,
    HairlinePending,
    InvalidDash,
    StylePriority,
    /// A selected clone's resolved source graph contains a shape planned Change.
    DependentCloneConflict,
    /// A selected clone's source graph is null/foreign/unresolvable/cyclic while
    /// changed shapes exist: conservative whole-plan refusal, never inferred safe.
    DependentCloneUncertain,
    StalePlan,
    InvalidTransaction,
    PostconditionMismatch,
    UnsupportedTextCoverage,
    UnsafeTextWhitespace, ///< partial native split/tidy cannot prove raw/collapsed content preservation
    UnrestorableTextPosition, ///< native tidy could merge/remove an authored positioned span
};

/// Read-only prospective numeric plan for one frozen logical text run. Plain
/// values and weak ownership only: it retains no layout iterator, no SPStyle
/// pointer and no dereferenceable document pointer. This is explicit pending
/// metadata on a top-level TextOwner member that stays
/// Excluded/TextAdapterPending; it is never itself a write instruction and the
/// member still carries no parent-level patch.
struct StrokeWidthTextRunPlan {
    StrokeWidthIntentKind intent_kind = StrokeWidthIntentKind::AbsoluteCssPx;
    /// Half-open logical character range, one record per frozen TextOwner
    /// `target.runs` entry in the same logical order.
    unsigned first_char = 0;
    unsigned last_char = 0;
    /// Weak rendering-source identity copied from the frozen SW1 run.
    SPWeakPtr<SPObject> style_source;
    /// Exact frozen SW1/A2 style snapshot, carried without re-reading the
    /// document; no SPStyle pointer and no normalized serialization.
    StrokeWidthStyle style;
    StrokeWidthEligibility eligibility = StrokeWidthEligibility::Eligible;
    StrokeWidthExclusion exclusion = StrokeWidthExclusion::None;
    /// Intended local-unit proposal; populated only when `outcome == Change` and
    /// the run is eligible. `native_dasharray_css`/`computed` mirror the shape
    /// member's canonical native dash result (present only with a changed array).
    std::optional<double> local_width;
    std::optional<std::vector<double>> local_dasharray;
    std::optional<double> local_dashoffset;
    std::optional<std::string> native_dasharray_css;
    /// The exact CSS text written for the width and the dash offset: the CSS
    /// number precision text-style writing may apply, so every text write path
    /// stores the same text and reads back `local_width`/`local_dashoffset`.
    std::optional<std::string> width_css;
    std::optional<std::string> dashoffset_css;
    std::optional<std::vector<double>> native_dasharray_computed;
    /// Independent inline !important retention, one per property.
    bool width_important = false;
    bool dasharray_important = false;
    bool dashoffset_important = false;
    StrokeWidthMemberOutcome outcome = StrokeWidthMemberOutcome::Unchanged;
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
};

/// Read-only preparation record for one eligible SW1 target. No SPCSSAttr is
/// allocated and no property is written; `local_width` and the dash fields are
/// the intended local-unit patch only when the outcome is `Change`.
struct StrokeWidthMemberPlan {
    StrokeWidthIntentKind intent_kind = StrokeWidthIntentKind::AbsoluteCssPx;
    StrokeWidthTarget target;                    ///< full SW1 snapshot
    double i2doc_affine[6] = {1, 0, 0, 1, 0, 0}; ///< exact i2doc entries
    /// Original inline style attribute: nullopt when absent, an empty string
    /// when present but empty. Never normalized.
    std::optional<std::string> inline_style;
    /// Authored XML/ownership snapshots for a later preflight/output check: the
    /// owner's weak original parent and its exact authored repr attributes
    /// (excluding only `style`), sorted by name. These prove neither rendered
    /// bounds nor a universal computed style. No native SPStyle::write(ALWAYS)
    /// snapshot is taken, so unrelated values and large numbers keep their
    /// authored representation instead of being normalized.
    SPWeakPtr<SPObject> original_parent;
    std::vector<std::pair<std::string, std::string>> non_style_attributes;
    /// Prospective per-run numeric plans for a TextOwner target: exactly one
    /// record per frozen `target.runs` entry in logical order, including runs
    /// whose own eligibility is excluded. Empty for every non-text member. The
    /// member itself stays Excluded/TextAdapterPending and carries no parent
    /// patch; these records are read-only pending metadata for a later native
    /// text adapter, never a write instruction.
    std::vector<StrokeWidthTextRunPlan> text_runs;
    std::optional<double> local_width;                  ///< desired local width
    std::optional<std::vector<double>> local_dasharray; ///< only when changed
    std::optional<double> local_dashoffset;             ///< only when changed
    /// Native canonical dash outcome, present only when `local_dasharray` is
    /// present (the member's dash array actually changed). `native_dasharray_css`
    /// is the raw numeric string fed to the native parser; it is kept even when
    /// that parser normalizes an all-zero pattern to empty, so a later write can
    /// still persist the authored zero pattern. `native_dasharray_computed` is
    /// the parser's computed result. Absent for disabled/old-zero/unchanged
    /// arrays; this phase never emits `stroke-dasharray:none`.
    std::optional<std::string> native_dasharray_css;
    std::optional<std::vector<double>> native_dasharray_computed;
    bool width_important = false; ///< retain inline !important in a later patch
    /// Independent inline !important retention for the two dash properties: a
    /// later native patch must be able to keep/declare each one separately.
    bool dasharray_important = false;
    bool dashoffset_important = false;
    StrokeWidthMemberOutcome outcome = StrokeWidthMemberOutcome::Unchanged;
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
};

/**
 * Read-only prepared width plan. It owns only weak ownership snapshots and
 * plain values: no SPDocument ownership, no dereferenceable document pointer
 * and no retained layout iterators. The weak document root invalidates on
 * destruction; callers own layout currency (SPDocument::ensureUpToDate) and the
 * opaque `scope_generation` identity. A later write phase must receive an
 * explicit live SPDocument& and a native caller token validated with
 * token.validFor(document) *before* the weak root identity is checked.
 */
struct StrokeWidthPlan {
    StrokeWidthPlanState state = StrokeWidthPlanState::Prepared;
    /// Whole-plan rejection reason (InvalidIntent/UnsupportedIntent/
    /// DependentCloneUncertain). None for a range rejection; read
    /// query.range_rejected/query.range_exclusion then. DependentCloneConflict
    /// is no longer produced: see following_clones.
    StrokeWidthMemberReason rejection = StrokeWidthMemberReason::None;
    /// On a DependentCloneUncertain whole-plan rejection, the concrete selected
    /// clone (SPUse) whose source graph could not be resolved. Weak and
    /// read-only, never dereferenced for a write.
    SPWeakPtr<SPItem> dependent_target;
    /// Selected clones whose original (source graph) the plan changes. Owner
    /// decision 2026-09-28: the rest of the selection is applied and these
    /// clones follow their original; they are never written themselves. The
    /// output checks require the same clones live.
    std::size_t following_clones = 0;
    /// Those clones, ordered by identity; weak, never written.
    std::vector<SPWeakPtr<SPItem>> following;
    SPWeakPtr<SPObject> document_root;              ///< weak root identity
    std::vector<SPWeakPtr<SPItem>> roots;           ///< captured weak roots
    std::optional<StrokeWidthTextRange> text_scope; ///< explicit range, if any
    bool combined_text_scope = false; ///< range plus other selected members
    std::uint64_t scope_generation = 0;             ///< caller-supplied, opaque
    /// Frozen copy of the requested intent, assigned even when the plan is
    /// Rejected, so a later apply phase can re-derive the requested values
    /// read-only instead of trusting frozen member patches. No semantics change
    /// in this phase.
    StrokeWidthIntent intent;
    StrokeWidthResult query;                        ///< full SW1 result
    std::size_t planned_changes = 0;
    std::size_t unchanged = 0;
    /// Query incompatible+unavailable+missing_sources plus per-member
    /// exclusions. Covered query duplicates stay in `query`, never here.
    std::size_t excluded = 0;
    /// Excluded text runs, independently of owner-level exclusions.
    std::size_t skipped_runs = 0;
    std::vector<StrokeWidthMemberPlan> members;
    /// Binding snapshots (`target` identity, parent, exact affine, authored attributes and inline
    /// style; no patch) of the selected clones the query excluded. They are never written; the
    /// output checks require them unchanged whatever class change a source write causes.
    std::vector<StrokeWidthMemberPlan> preserved_clones;
};

/// Prepare numeric shape width patches for explicit roots. Read-only.
StrokeWidthPlan prepare_stroke_widths(SPDocument &document,
                                      std::vector<SPItem *> const &roots,
                                      StrokeWidthIntent const &intent,
                                      std::uint64_t scope_generation);

/// Range overload mirroring the query. An invalid range rejects with the query
/// reason and never falls back to `roots`.
StrokeWidthPlan prepare_stroke_widths(SPDocument &document,
                                      StrokeWidthTextRange const &text_range,
                                      std::vector<SPItem *> const &roots,
                                      StrokeWidthIntent const &intent,
                                      std::uint64_t scope_generation);

/// Explicit range for its text owner plus independently selected compatible members.
StrokeWidthPlan prepare_stroke_widths_combined(SPDocument &document,
                                               StrokeWidthTextRange const &text_range,
                                               std::vector<SPItem *> const &roots,
                                               StrokeWidthIntent const &intent,
                                               std::uint64_t scope_generation);

// ---------------------------------------------------------------------------
// Caller-owned numeric shape writer (SW2B).
//
// The caller retains the stable `DocumentUndo::RollbackableInteraction` token and
// the document owner throughout the synchronous operation: it opens a discrete
// action with `beginAtomicInteraction`, calls apply/output-ready, and alone
// decides rollback or `commitAtomically(..., ready)`. These entries never
// create/settle a token, transaction or history entry, never touch selection or
// defaults, and never call done/maybeDone/commit/rollback. Token `validFor` is
// an identity/readiness check, not rollback ownership. A future live caller
// holds the native operation lease for the duration; no lease is acquired here.
// ---------------------------------------------------------------------------

/// Outcome of one bounded apply attempt.
enum class StrokeWidthApplyState {
    Applied,   ///< every planned change was written and the intended output verified
    Unchanged, ///< nothing to write (all members already desired or excluded)
    Rejected,  ///< no controller write occurred (invalid token, stale/rejected plan)
    Failed,    ///< a write may have occurred; the caller REQUIRES rollback
};

/// Observed result. They are not history publication. `changed` counts only
/// members whose intended output was verified: on `Failed` it is always 0, even
/// though `attempted_writes` native `changeCSS` calls may already have been
/// issued and the caller's rollback is therefore pending. `attempted_writes` is
/// the number of native `changeCSS` calls issued (0 for Rejected/Unchanged) and
/// equals the verified `changed` count on Applied. No failed attempt claims an
/// accepted change or publication. `reason` is None on success.
/// Deterministic work counters of one apply, for complexity tests (never wall-clock).
/// Filled by every apply entry point, also on Failed/Rejected.
struct StrokeWidthWorkCounters {
    /// Owners inspected by the between-write pending-owner scan.
    std::size_t pending_owner_checks = 0;
    /// Per-character rendered-style captures (query-equivalent work on text).
    std::size_t char_captures = 0;
    /// Native `sp_te_apply_style` range writes (each tidies the XML and rebuilds the layout).
    std::size_t native_range_writes = 0;
    /// Direct `changeCSS` writes on a single element (owner or complete plain span).
    std::size_t direct_source_writes = 0;
    /// Layout rebuilds issued by the controller or implied by a native range write.
    std::size_t layout_rebuilds = 0;
    /// Character-to-source lookups of the direct large-owner writer: one per character when its
    /// per-owner map is (re)built and one per character of each run it checks. Bounded by a small
    /// multiple of the owner's character count while the map is built once per owner.
    std::size_t source_lookups = 0;
};

struct StrokeWidthApplyResult {
    StrokeWidthApplyState state = StrokeWidthApplyState::Unchanged;
    StrokeWidthMemberReason reason = StrokeWidthMemberReason::None;
    std::size_t changed = 0;
    std::size_t unchanged = 0;
    std::size_t excluded = 0;
    std::size_t attempted_writes = 0;
    std::size_t skipped_runs = 0;
    /// Distinct verified changed owners with stroke paint none after writing.
    std::size_t paint_none = 0;
    StrokeWidthWorkCounters work;
};

/**
 * Detects changes to the document XML made by anyone but the text writer itself, so the
 * between-write scan of the still-pending owners runs only after such a change (or a write whose
 * effects the watch cannot attribute) instead of before every write. Declared here so tests can
 * drive it; not a general facility.
 *
 * It observes the whole document tree, so ancestor protection/transform changes, stylesheet
 * edits and reparenting of a pending owner are seen like edits inside the owner. Notifications
 * during one of our own writes are ignored only inside that write's scope: the owner subtree for
 * a native range write (the writer may split and tidy spans), or exactly ONE `style` attribute
 * notification of one element for a direct write. Anything else in a direct write's scope,
 * including a second rewrite of the active element's style, is a violation the caller must
 * refuse (`end_write` returns false).
 */
class PendingOwnerWatch final : public XML::NodeObserver
{
public:
    explicit PendingOwnerWatch(SPDocument &document);
    PendingOwnerWatch(PendingOwnerWatch const &) = delete;
    PendingOwnerWatch &operator=(PendingOwnerWatch const &) = delete;
    ~PendingOwnerWatch() override;

    /// True until acknowledged: an unattributed change happened (initially true, so the first
    /// write is preceded by one full scan).
    bool foreign() const { return _foreign; }
    void acknowledge() { _foreign = false; }

    void begin_subtree_write(XML::Node const *root);
    void begin_style_write(XML::Node const *element);
    /// Ends the write scope. False when a direct write's scope saw anything but its one expected
    /// style notification.
    bool end_write();

    void notifyChildAdded(XML::Node &node, XML::Node &, XML::Node *) override { changed(node, 0); }
    void notifyChildRemoved(XML::Node &node, XML::Node &, XML::Node *) override { changed(node, 0); }
    void notifyChildOrderChanged(XML::Node &node, XML::Node &, XML::Node *, XML::Node *) override
    {
        changed(node, 0);
    }
    void notifyContentChanged(XML::Node &node, Util::ptr_shared, Util::ptr_shared) override { changed(node, 0); }
    void notifyAttributeChanged(XML::Node &node, GQuark name, Util::ptr_shared, Util::ptr_shared) override
    {
        changed(node, name);
    }
    void notifyElementNameChanged(XML::Node &node, GQuark, GQuark) override { changed(node, 0); }

private:
    void changed(XML::Node &node, GQuark attribute);
    void detach();

    XML::Node *_root = nullptr;
    GQuark _style = 0;
    sigc::connection _destroyed;
    XML::Node const *_subtree = nullptr;
    XML::Node const *_element = nullptr;
    unsigned _expected_style_writes = 0;
    bool _violated = false;
    bool _foreign = true;
};

/// Test seam: the number of write units of one text owner from which the direct large-owner
/// writer is used (default 16). SIZE_MAX disables only that bulk optimization, so tests can
/// compare it with the native range write; the direct writes that already existed (a plain
/// whole-owner text, a complete span with a stylesheet width) can still happen. Process-wide;
/// restore the previous value.
std::size_t stroke_width_set_bulk_text_threshold(std::size_t units);

/**
 * Strict comparison of two prepared plans by everything that decides what is
 * written: intent, counts, text scope, each target's frozen style (hairline,
 * vector-effect, dash, priority) and run scope, and every member and text-run
 * patch (width, dash array and offset, native dash text, importance bits, outcome).
 * Parent, exact affine, inline style text and authored attributes are compared
 * too; the only permitted difference is `d`, the output of an applied path effect
 * whose reference and authored source geometry (`inkscape:original-d`) are identical:
 * an automatic update regenerating it.
 */
bool stroke_width_plans_match(StrokeWidthPlan const &a, StrokeWidthPlan const &b);

// Every apply entry point takes an optional live-scope predicate. It is checked
// before the first write and before/after every later member or run write; on
// false the apply stops and returns Failed (the caller rolls back). Empty: always live.

/**
 * Apply one prepared numeric-shape plan using fresh native SPCSSAttr objects and
 * `SPObject::changeCSS`. Read-only re-verification runs before the first write;
 * after actual writes native layout currency is refreshed inside the caller's
 * transaction. A `Failed` result may leave pending partial XML and requires the
 * caller to roll back its own token.
 */
StrokeWidthApplyResult apply_stroke_widths(SPDocument &document, StrokeWidthPlan const &plan,
                                           std::uint64_t current_scope_generation,
                                           DocumentUndo::RollbackableInteraction &interaction,
                                           std::function<bool()> const &live_scope = {});

/**
 * Status note in order: changed owners with stroke paint none, linked clones
 * that follow their changed original (plan.following_clones), then skipped items
 * (\a excluded is the apply result's excluded count; following clones that
 * are excluded records are not counted as skipped), including skipped text runs.
 * Applied is silent without any clauses; otherwise prefixed "Stroke width applied".
 * RemoveStroke never reports its own none paint and uses the prefix "Stroke removed".
 * Unchanged reports the additive minimum-step floor or skipped items, otherwise
 * it is silent. Paint-none counts only runs/members with outcome Change.
 * Prefer the outcome-aware overload when the apply result is available.
 */
std::string stroke_width_applied_note(StrokeWidthPlan const &plan, std::size_t excluded);
/// Outcome-aware form: reports paint-none only for a verified Applied result;
/// Failed and Rejected are silent.
std::string stroke_width_applied_note(StrokeWidthPlan const &plan, StrokeWidthApplyResult const &result);

// Shared width-row policy; no GTK or document access. Unsupported/non-linear
// units have no step (0), 3 digits, and the point preset list.
double stroke_width_step(Util::Unit const &unit);
unsigned stroke_width_digits(Util::Unit const &unit);
Util::Unit const *stroke_width_list_unit(Util::Unit const &unit);
enum class StrokeWidthPresetKind { Absolute, Hairline };
struct StrokeWidthPreset {
    std::string label;
    Util::Unit const *unit = nullptr;
    double value = 0;
    StrokeWidthPresetKind kind = StrokeWidthPresetKind::Absolute;
};
std::vector<StrokeWidthPreset> stroke_width_presets(Util::Unit const &list_unit);
double stroke_width_preset_px(StrokeWidthPreset const &preset);
bool stroke_width_same_width(double a_px, double b_px);
bool stroke_width_can_decrease(double w_px, double step_px);

/**
 * Read-only output-readiness check for a caller-owned atomic commit: verifies
 * token/document/scope/binding and that the document shows the intended result.
 * Never writes, settles, updates layout or emits callbacks. Safe to call again
 * inside `commitAtomically`'s ready callback.
 */
bool stroke_widths_output_ready(SPDocument &document, StrokeWidthPlan const &plan,
                                std::uint64_t current_scope_generation,
                                DocumentUndo::RollbackableInteraction &interaction);

// ---------------------------------------------------------------------------
// Caller-owned text-only width writer (SW3-B2).
//
// This is a controller-level numeric writer for selected TextOwners' existing
// `member.text_runs` plan records. It does not promote the TextOwner members out
// of its Excluded/TextAdapterPending state, does not alter the shape plan or the
// shape writer, and never creates or settles a transaction: like the shape
// writer the caller owns the single `DocumentUndo::RollbackableInteraction`,
// opens it with `beginAtomicInteraction`, and alone commits or rolls back.
//
// Supported today (deliberately narrow, by design):
//  - one or more TextOwner members, whole-owner or explicit partial range;
//  - one or more eligible Ordinary-stroke runs, each a non-empty half-open
//    range of real glyphs;
//  - absolute CSS px or relative percent width intent.
//
// Explicitly NOT supported in this slice (whole-plan rejection before any
// native write, never a silent partial application):
//  - any changed member whose kind is not TextOwner (mixed shape+text plans);
//  - hairline or non-scaling text runs;
//  - any changed dash patch (`scale_dashes` pending a text dash contract);
//  - zero-length or non-glyph-only changed runs;
//  - clones (their adapter remains pending).
// There is no fallback to the parent CSS: unsupported cases reject instead.
// Native span splitting may change style-source identity/run grouping, so the
// post-write verifier reads rendered per-character styles and semantic
// content/count, never a stale SPString, layout iterator or style pointer.
//
// `StrokeWidthApplyResult::changed` counts verified TextOwner members, not
// runs; `attempted_writes` counts the native `sp_te_apply_style` calls issued.
// A `Failed` result means a write may have landed and the caller must roll back
// the same token. This function never calls done/maybeDone/commit/rollback.
// ---------------------------------------------------------------------------

/**
 * Apply one prepared text plan through `sp_te_apply_style` with fresh
 * `SPCSSAttr` objects, writing eligible runs in descending logical order and
 * reacquiring layout/iterators before every call. Read-only preflight
 * re-verification runs before the first write.
 */
StrokeWidthApplyResult apply_stroke_widths_text(SPDocument &document, StrokeWidthPlan const &plan,
                                                std::uint64_t current_scope_generation,
                                                DocumentUndo::RollbackableInteraction &interaction,
                                                std::function<bool()> const &live_scope = {});

/**
 * Read-only text output-readiness check for a caller-owned atomic commit:
 * verifies token/document/scope/binding, whole-owner semantic text/count, every
 * run covered by the frozen plan, and every glyph outside an explicit partial
 * range against the immutable whole-owner baseline frozen in the plan at query
 * time. A changed glyph is never required to equal that baseline. It never
 * writes, settles, updates layout or emits callbacks, and is safe to call again
 * inside `commitAtomically`'s ready callback.
 */
bool stroke_widths_text_output_ready(SPDocument &document, StrokeWidthPlan const &plan,
                                     std::uint64_t current_scope_generation,
                                     DocumentUndo::RollbackableInteraction &interaction);

// ---------------------------------------------------------------------------
// Caller-owned compatible shape+text width writer (SW3-C).
//
// Separate opt-in entry point that applies one absolute/relative width intent to
// every compatible selected member of a single whole-object plan in one
// operation: eligible Shape members and eligible TextOwner members. It is not a
// composition of `apply_stroke_widths` and `apply_stroke_widths_text`; those two
// each freeze and re-derive the whole plan at entry, so calling them in sequence
// makes the second see the first writer's changes as a stale plan.
//
// Semantics:
//  - the plan is frozen exactly once with a single whole-plan `reprepare_matches`
//    before the first native write, including for a no-op;
//  - a structural gate and a whole-plan preflight (`preflight_text_plan` plus the
//    per-shape write-ready checks) run before the first write, so an unsupported
//    or stale plan rejects atomically with zero native calls;
//  - the deterministic order is every changed text run first (descending logical
//    start within each owner, layout/iterators reacquired per call), then every
//    changed Shape member in frozen plan order, each rechecked before its write;
//  - one `document.ensureUpToDate()` and one read-only combined verifier run
//    after all writes; nothing is reprepared or rebaselined after a write.
//
// Supported: whole-object scope or an explicit combined text range plus other
// selected members; Shape `Change` with a single-run width patch;
// eligible Ordinary-stroke TextOwner runs obeying the text-only writer's rules.
// Query-excluded bitmaps are never written by this operation; their captured
// exclusion classification is rechecked, and normal-path byte preservation is
// covered by an outcome test. The query does not snapshot bitmap payload bytes.
//
// Explicitly unsupported, rejected before any write with no fallback:
//  - a legacy text-only range plan supplied to this entry point (use the
//    explicit combined preparation path for a range plus selected members);
//  - a changed member that is not a Shape or a TextOwner, or a Shape change that
//    is not a supported single-run patch;
//  - hairline/non-scaling or dash-patched eligible text runs (the text-only
//    rules, via `text_runs_supported`);
//  - a selected-clone dependency conflict/uncertainty (already a plan-level
//    rejection before this entry point is reached).
//
// `changed` counts observed changed owners (shape `planned_changes` plus
// TextOwner owners with changed runs); `attempted_writes` counts actual native
// calls (`sp_te_apply_style` + `changeCSS`). On any postwrite error the result is
// `Failed` with `changed = 0`, so the caller rolls back the same token. This
// function never creates, settles, commits or rolls back a transaction and never
// touches selection or defaults.
// ---------------------------------------------------------------------------

/**
 * Apply one prepared whole-object mixed plan. One frozen preflight before the
 * first write, text-run phase then shape phase, one `ensureUpToDate` and one
 * combined read-only verifier after the writes. No reprepare between writes.
 */
StrokeWidthApplyResult apply_stroke_widths_compatible(SPDocument &document,
                                                     StrokeWidthPlan const &plan,
                                                     std::uint64_t current_scope_generation,
                                                     DocumentUndo::RollbackableInteraction &interaction,
                                                     std::function<bool()> const &live_scope = {});

/// Bound whole-owner adapter: Remove appends its frozen stroke declaration,
/// preserving every unrelated inline byte (including !important placement).
/// Same caller-owned token, guards and verifier; explicit ranges are refused.
StrokeWidthApplyResult apply_stroke_widths_bound(SPDocument &document, StrokeWidthPlan const &plan,
                                                std::uint64_t current_scope_generation,
                                                DocumentUndo::RollbackableInteraction &interaction,
                                                std::function<bool()> const &live_scope = {});

/**
 * Read-only combined output-readiness check for a caller-owned atomic commit:
 * validates token/document/scope/binding and runs the same combined verifier as
 * `apply_stroke_widths_compatible`. Never writes, settles, updates layout or
 * emits callbacks; safe to call again inside `commitAtomically`'s ready callback.
 */
bool stroke_widths_compatible_output_ready(SPDocument &document, StrokeWidthPlan const &plan,
                                           std::uint64_t current_scope_generation,
                                           DocumentUndo::RollbackableInteraction &interaction);

/**
 * Begin the atomic interaction of one stroke-width change for \a plan.
 * Automatic updates (a path effect rewriting its path after Undo, a filter
 * region computed on load) can leave XML changes outside the history, which
 * an atomic interaction refuses. When the document is quiescent (no edit,
 * file operation or close in progress) and \a plan will change something,
 * they are settled first as their own Undo step ("Automatic update") and the
 * interaction is begun again. Otherwise it refuses as before, so a no-op
 * request never adds a step or drops Redo. The settlement's commit callbacks may
 * destroy the document: afterwards the document's destruction and the optional
 * \a proceed predicate (false: the caller's scope ended) are checked before another
 * interaction is begun, and nullopt is returned.
 */
std::optional<DocumentUndo::RollbackableInteraction> begin_stroke_width_interaction(
    SPDocument &document, StrokeWidthPlan const &plan, std::function<bool()> const &proceed = {});

} // namespace Inkscape::UI

#endif // INKSCAPE_UI_STROKE_WIDTH_CONTROLLER_H
