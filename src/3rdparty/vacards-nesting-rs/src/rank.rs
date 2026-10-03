// SPDX-License-Identifier: GPL-2.0-or-later

//! Canonical, deterministic ordering for complete nesting solutions.
//!
//! A single ordering is deliberately shared by constructive and experimental
//! backends. Geometry validity is supplied by the caller after authoritative
//! Jagua validation; malformed result records are always ranked invalid here
//! as a second fail-closed guard.

use crate::job::{Part, Polygon, SolverInput, VacNestingPlacement};
use std::cmp::Ordering;

// Drop 22 of the 52 stored mantissa bits, retaining approximately 30 binary
// fraction bits (relative resolution below 1e-9 for normal values). Unlike a
// pairwise epsilon comparison, assigning every metric to one canonical bucket
// is transitive and therefore safe for order-independent reductions.
const METRIC_MANTISSA_DROP_BITS: u32 = 22;
const METRIC_BUCKET_MASK: u64 = !((1_u64 << METRIC_MANTISSA_DROP_BITS) - 1);
const METRIC_BUCKET_HALF: u64 = 1_u64 << (METRIC_MANTISSA_DROP_BITS - 1);
const MAX_FINITE_BITS: u64 = f64::MAX.to_bits();

#[derive(Clone, Debug)]
pub(crate) struct CanonicalRank {
    valid: bool,
    placed_area: f64,
    placed_count: u32,
    occupied_area: f64,
    stable_key: Vec<PlacementKey>,
}

#[derive(Clone, Copy, Debug)]
struct PlacementKey {
    placed: bool,
    rotation_degrees: f64,
    translation_y: f64,
    translation_x: f64,
    part_id: u64,
    exact_rotation_bits: u64,
    exact_translation_y_bits: u64,
    exact_translation_x_bits: u64,
}

impl CanonicalRank {
    pub(crate) fn evaluate(
        input: &SolverInput,
        results: &[VacNestingPlacement],
        geometry_valid: bool,
    ) -> Self {
        if !geometry_valid || !records_are_well_formed(input, results) {
            return Self::invalid();
        }

        let mut placed_area = CompensatedSum::default();
        let mut placed_count = 0_u32;
        let mut bounds = (
            f64::INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::NEG_INFINITY,
        );
        // Result records are presented in caller input order, which is not a
        // stable arithmetic order. Sort references by the immutable part ID
        // before accumulating objectives. This is important even though area
        // is bucketed later: non-associative floating-point addition can move
        // a sum across a bucket boundary when a caller permutes the same
        // parts.
        let mut ordered = input.parts.iter().zip(results).collect::<Vec<_>>();
        ordered.sort_unstable_by_key(|(part, _)| part.id);
        if ordered.windows(2).any(|pair| pair[0].0.id == pair[1].0.id) {
            return Self::invalid();
        }

        let mut stable_key = Vec::with_capacity(results.len());
        for (part, placement) in ordered {
            let placed = placement.placed != 0;
            stable_key.push(PlacementKey {
                placed,
                rotation_degrees: canonical_rotation(placement.rotation_degrees),
                translation_y: canonical_zero(placement.translation_y),
                translation_x: canonical_zero(placement.translation_x),
                part_id: placement.part_id,
                exact_rotation_bits: placement.rotation_degrees.to_bits(),
                exact_translation_y_bits: placement.translation_y.to_bits(),
                exact_translation_x_bits: placement.translation_x.to_bits(),
            });
            if !placed {
                continue;
            }

            placed_area.add(part_area(part));
            placed_count = placed_count.saturating_add(1);
            extend_transformed_bounds(part, placement, &mut bounds);
        }

        let occupied_area = if placed_count == 0 {
            f64::INFINITY
        } else {
            (bounds.2 - bounds.0).max(0.0) * (bounds.3 - bounds.1).max(0.0)
        };

        Self {
            valid: true,
            placed_area: placed_area.finish(),
            placed_count,
            occupied_area,
            stable_key,
        }
    }

    pub(crate) fn invalid() -> Self {
        Self {
            valid: false,
            placed_area: 0.0,
            placed_count: 0,
            occupied_area: f64::INFINITY,
            stable_key: Vec::new(),
        }
    }

    /// Returns `Greater` when `self` is the preferable solution.
    pub(crate) fn quality_cmp(&self, other: &Self) -> Ordering {
        match self.valid.cmp(&other.valid) {
            Ordering::Equal => {}
            ordering => return ordering,
        }
        if !self.valid {
            return Ordering::Equal;
        }

        match metric_bucket(self.placed_area).cmp(&metric_bucket(other.placed_area)) {
            Ordering::Equal => {}
            ordering => return ordering,
        }
        match self.placed_count.cmp(&other.placed_count) {
            Ordering::Equal => {}
            ordering => return ordering,
        }
        match metric_bucket(other.occupied_area).cmp(&metric_bucket(self.occupied_area)) {
            Ordering::Equal => {}
            ordering => return ordering,
        }

        // A stable-ID-sorted, exact final key makes reduction independent of
        // discovery and caller input order once bucketed objectives tie.
        compare_stable_keys(&self.stable_key, &other.stable_key)
    }

    pub(crate) fn is_better_than(&self, other: &Self) -> bool {
        self.quality_cmp(other) == Ordering::Greater
    }

    pub(crate) fn is_valid(&self) -> bool {
        self.valid
    }

    #[cfg(test)]
    pub(crate) fn placed_area(&self) -> f64 {
        self.placed_area
    }

    /// Public progress uses the same transitive area bucket as solution
    /// reduction. Two raw sums inside one bucket therefore publish one score,
    /// so accepting a higher-count winner can never make `best_score` move
    /// backwards by a sub-bucket rounding amount.
    pub(crate) fn canonical_area_score(&self) -> f64 {
        f64::from_bits(metric_bucket(self.placed_area))
    }

    pub(crate) fn placed_count(&self) -> u32 {
        self.placed_count
    }
}

/// Neumaier compensated summation in a caller-independent order. The
/// compensation is deterministic because `CanonicalRank::evaluate` feeds it
/// values sorted by stable part ID.
#[derive(Default)]
struct CompensatedSum {
    sum: f64,
    correction: f64,
}

impl CompensatedSum {
    fn add(&mut self, value: f64) {
        let next = self.sum + value;
        if self.sum.abs() >= value.abs() {
            self.correction += (self.sum - next) + value;
        } else {
            self.correction += (value - next) + self.sum;
        }
        self.sum = next;
    }

    fn finish(self) -> f64 {
        self.sum + self.correction
    }
}

fn records_are_well_formed(input: &SolverInput, results: &[VacNestingPlacement]) -> bool {
    results.len() == input.parts.len()
        && input.parts.iter().zip(results).all(|(part, placement)| {
            placement.part_id == part.id
                && placement.placed <= 1
                && placement.translation_x.is_finite()
                && placement.translation_y.is_finite()
                && placement.rotation_degrees.is_finite()
                && placement.reserved == [0; 7]
                && (placement.placed != 0
                    || (placement.translation_x == 0.0
                        && placement.translation_y == 0.0
                        && placement.rotation_degrees == 0.0))
        })
}

fn extend_transformed_bounds(
    part: &Part,
    placement: &VacNestingPlacement,
    bounds: &mut (f64, f64, f64, f64),
) {
    let angle = placement.rotation_degrees.to_radians();
    let (sin, cos) = angle.sin_cos();
    for component in &part.components {
        for point in &component.outer.0 {
            let x = point.x * cos - point.y * sin + placement.translation_x;
            let y = point.x * sin + point.y * cos + placement.translation_y;
            bounds.0 = bounds.0.min(x);
            bounds.1 = bounds.1.min(y);
            bounds.2 = bounds.2.max(x);
            bounds.3 = bounds.3.max(y);
        }
    }
}

fn part_area(part: &Part) -> f64 {
    part.components
        .iter()
        .map(|component| {
            polygon_area(&component.outer) - component.holes.iter().map(polygon_area).sum::<f64>()
        })
        .sum::<f64>()
        .max(0.0)
}

fn polygon_area(polygon: &Polygon) -> f64 {
    polygon
        .0
        .iter()
        .zip(polygon.0.iter().cycle().skip(1))
        .take(polygon.0.len())
        .map(|(left, right)| left.x * right.y - right.x * left.y)
        .sum::<f64>()
        .abs()
        * 0.5
}

fn metric_bucket(value: f64) -> u64 {
    debug_assert!(!value.is_nan() && value >= 0.0);
    if value == 0.0 {
        return 0;
    }
    if value == f64::INFINITY {
        return f64::INFINITY.to_bits();
    }

    // Positive IEEE-754 values have monotonically ordered bit patterns. Round
    // to the nearest fixed-width mantissa bucket using integer arithmetic, so
    // the result does not depend on libm or platform floating-point modes.
    let bits = value.to_bits();
    let rounded = bits.saturating_add(METRIC_BUCKET_HALF);
    if rounded > MAX_FINITE_BITS {
        MAX_FINITE_BITS & METRIC_BUCKET_MASK
    } else {
        rounded & METRIC_BUCKET_MASK
    }
}

fn compare_stable_keys(left: &[PlacementKey], right: &[PlacementKey]) -> Ordering {
    for (left, right) in left.iter().zip(right) {
        // Keys are sorted by stable part ID. Prefer placing the lower-ID part
        // when all canonical objectives tie, then the smaller rigid transform.
        match left.placed.cmp(&right.placed) {
            Ordering::Equal => {}
            ordering => return ordering,
        }
        if left.placed {
            for ordering in [
                right.rotation_degrees.total_cmp(&left.rotation_degrees),
                right.translation_y.total_cmp(&left.translation_y),
                right.translation_x.total_cmp(&left.translation_x),
            ] {
                if ordering != Ordering::Equal {
                    return ordering;
                }
            }
        }
        match right.part_id.cmp(&left.part_id) {
            Ordering::Equal => {}
            ordering => return ordering,
        }
        for ordering in [
            right.exact_rotation_bits.cmp(&left.exact_rotation_bits),
            right
                .exact_translation_y_bits
                .cmp(&left.exact_translation_y_bits),
            right
                .exact_translation_x_bits
                .cmp(&left.exact_translation_x_bits),
        ] {
            if ordering != Ordering::Equal {
                return ordering;
            }
        }
    }
    right.len().cmp(&left.len())
}

fn canonical_rotation(rotation_degrees: f64) -> f64 {
    let normalized = rotation_degrees.rem_euclid(360.0);
    if normalized == 0.0 || (360.0 - normalized).abs() <= 1.0e-10 {
        0.0
    } else {
        normalized
    }
}

fn canonical_zero(value: f64) -> f64 {
    if value == 0.0 { 0.0 } else { value }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::job::{PartComponent, VacNestingOptions, VacNestingPoint};

    #[test]
    fn canonical_order_is_validity_area_count_compactness_then_stability() {
        let input = input_with_parts(&[(1, 10.0, 10.0), (2, 4.0, 10.0), (3, 4.0, 10.0)]);
        let empty = placements(&input, &[]);
        let one_large = placements(&input, &[(0, 0.0, 0.0)]);
        let two_small = placements(&input, &[(1, 0.0, 0.0), (2, 10.0, 0.0)]);
        let shifted_large = placements(&input, &[(0, 100.0, 100.0)]);

        let invalid = CanonicalRank::evaluate(&input, &one_large, false);
        let empty_rank = CanonicalRank::evaluate(&input, &empty, true);
        let large_rank = CanonicalRank::evaluate(&input, &one_large, true);
        let small_rank = CanonicalRank::evaluate(&input, &two_small, true);
        assert!(empty_rank.is_better_than(&invalid));
        assert!(large_rank.is_better_than(&small_rank));

        // Translation does not alter the occupied area of one part, so the
        // stable lower transform wins after all objective values tie.
        assert!(large_rank.is_better_than(&CanonicalRank::evaluate(&input, &shifted_large, true)));
    }

    #[test]
    fn stable_key_prefers_lower_part_id_when_objectives_tie() {
        let input = input_with_parts(&[(1, 10.0, 10.0), (2, 10.0, 10.0)]);
        let earlier = placements(&input, &[(0, 0.0, 0.0)]);
        let later = placements(&input, &[(1, 0.0, 0.0)]);
        let earlier_rank = CanonicalRank::evaluate(&input, &earlier, true);
        let later_rank = CanonicalRank::evaluate(&input, &later, true);
        assert!(earlier_rank.is_better_than(&later_rank));
        assert!(!later_rank.is_better_than(&earlier_rank));
    }

    #[test]
    fn permuting_input_and_results_preserves_rank_and_admitted_winner() {
        let input = input_with_parts(&[(30, 10.0, 10.0), (10, 10.0, 10.0), (20, 10.0, 10.0)]);
        let lower_id = placements(&input, &[(1, 0.0, 0.0)]);
        let higher_id = placements(&input, &[(2, 0.0, 0.0)]);

        let permutation = [2, 0, 1];
        let permuted_input = permute_input(&input, &permutation);
        let permuted_lower_id = permute_results(&lower_id, &permutation);
        let permuted_higher_id = permute_results(&higher_id, &permutation);

        let lower_rank = CanonicalRank::evaluate(&input, &lower_id, true);
        let higher_rank = CanonicalRank::evaluate(&input, &higher_id, true);
        let permuted_lower_rank =
            CanonicalRank::evaluate(&permuted_input, &permuted_lower_id, true);
        let permuted_higher_rank =
            CanonicalRank::evaluate(&permuted_input, &permuted_higher_id, true);

        assert_eq!(
            lower_rank.quality_cmp(&permuted_lower_rank),
            Ordering::Equal
        );
        assert_eq!(
            higher_rank.quality_cmp(&permuted_higher_rank),
            Ordering::Equal
        );
        assert!(lower_rank.is_better_than(&higher_rank));
        assert!(permuted_lower_rank.is_better_than(&permuted_higher_rank));
    }

    #[test]
    fn disparate_area_sum_is_bit_identical_under_input_permutation() {
        // At 1e16, adding unit-area parts in caller order loses a different
        // number of low bits depending on the permutation. Stable-ID ordered,
        // compensated summation must describe the same logical selection with
        // exactly the same objective bits.
        let input = input_with_parts(&[
            (30, 1.0, 1.0),
            (10, 10_000_000_000_000_000.0, 1.0),
            (20, 1.0, 1.0),
        ]);
        let all = placements(&input, &[(0, 0.0, 0.0), (1, 2.0, 0.0), (2, 4.0, 0.0)]);
        let permutation = [1, 0, 2];
        let permuted_input = permute_input(&input, &permutation);
        let permuted_all = permute_results(&all, &permutation);

        let rank = CanonicalRank::evaluate(&input, &all, true);
        let permuted_rank = CanonicalRank::evaluate(&permuted_input, &permuted_all, true);
        assert_eq!(
            rank.placed_area().to_bits(),
            permuted_rank.placed_area().to_bits()
        );
        assert_eq!(
            rank.canonical_area_score().to_bits(),
            permuted_rank.canonical_area_score().to_bits()
        );
        assert_eq!(rank.quality_cmp(&permuted_rank), Ordering::Equal);
    }

    #[test]
    fn canonical_progress_score_is_constant_inside_an_area_bucket() {
        let lower = synthetic_rank(1.0, 1);
        let upper = synthetic_rank(
            f64::from_bits(1.0_f64.to_bits() + METRIC_BUCKET_HALF / 2),
            2,
        );
        assert!(lower.placed_area() < upper.placed_area());
        assert_eq!(
            metric_bucket(lower.placed_area()),
            metric_bucket(upper.placed_area())
        );
        assert_eq!(
            lower.canonical_area_score().to_bits(),
            upper.canonical_area_score().to_bits()
        );
    }

    #[test]
    fn duplicate_part_ids_rank_invalid() {
        let input = input_with_parts(&[(7, 10.0, 10.0), (7, 5.0, 5.0)]);
        let results = placements(&input, &[(0, 0.0, 0.0)]);
        assert!(!CanonicalRank::evaluate(&input, &results, true).is_valid());
    }

    #[test]
    fn stable_key_exactly_orders_semantically_canonical_transform_aliases() {
        let input = input_with_parts(&[(1, 10.0, 10.0)]);
        let canonical = placements(&input, &[(0, 0.0, 0.0)]);
        let mut alias = canonical.clone();
        alias[0].rotation_degrees = 360.0 - 0.5e-10;
        alias[0].translation_x = -0.0;

        let canonical_rank = CanonicalRank::evaluate(&input, &canonical, true);
        let alias_rank = CanonicalRank::evaluate(&input, &alias, true);
        assert!(canonical_rank.is_better_than(&alias_rank));
        assert!(!alias_rank.is_better_than(&canonical_rank));
    }

    #[test]
    fn malformed_records_rank_invalid() {
        let input = input_with_parts(&[(7, 10.0, 10.0)]);
        let valid = placements(&input, &[(0, -0.0, 0.0)]);
        let rank = CanonicalRank::evaluate(&input, &valid, true);
        assert!(rank.is_valid());

        let mut wrong_id = valid.clone();
        wrong_id[0].part_id = 99;
        assert!(!CanonicalRank::evaluate(&input, &wrong_id, true).is_valid());
        let mut non_finite = valid;
        non_finite[0].translation_x = f64::NAN;
        assert!(!CanonicalRank::evaluate(&input, &non_finite, true).is_valid());
    }

    #[test]
    fn occupied_bounds_include_every_rigid_component() {
        let mut input = input_with_parts(&[(1, 10.0, 10.0)]);
        input.parts[0].components.push(PartComponent {
            outer: rectangle(20.0, 0.0, 5.0, 5.0),
            holes: Vec::new(),
        });
        let rank = CanonicalRank::evaluate(&input, &placements(&input, &[(0, 0.0, 0.0)]), true);
        assert_eq!(rank.placed_count(), 1);
        assert!((rank.placed_area() - 125.0).abs() < 1.0e-9);
        assert!((rank.occupied_area - 250.0).abs() < 1.0e-9);
    }

    #[test]
    fn tolerance_boundary_triple_is_transitive_and_permutation_stable() {
        // A pairwise relative epsilon of 1e-9 made A equivalent to B and B
        // equivalent to C while still ordering A below C. That relation is
        // not transitive and allowed incumbent reduction to depend on the
        // discovery order. Canonical buckets must impose one strict chain.
        let candidates = [
            synthetic_rank(1.0, 3),
            synthetic_rank(1.0 + 0.75e-9, 2),
            synthetic_rank(1.0 + 1.50e-9, 1),
        ];

        assert_eq!(candidates[0].quality_cmp(&candidates[1]), Ordering::Less);
        assert_eq!(candidates[1].quality_cmp(&candidates[2]), Ordering::Less);
        assert_eq!(candidates[0].quality_cmp(&candidates[2]), Ordering::Less);

        for left in &candidates {
            for right in &candidates {
                assert_eq!(
                    left.quality_cmp(right),
                    right.quality_cmp(left).reverse(),
                    "canonical comparison must be antisymmetric"
                );
                for third in &candidates {
                    if left.quality_cmp(right) != Ordering::Less
                        && right.quality_cmp(third) != Ordering::Less
                    {
                        assert_ne!(
                            left.quality_cmp(third),
                            Ordering::Less,
                            "canonical comparison must be transitive"
                        );
                    }
                }
            }
        }

        let permutations = [
            [0, 1, 2],
            [0, 2, 1],
            [1, 0, 2],
            [1, 2, 0],
            [2, 0, 1],
            [2, 1, 0],
        ];
        for permutation in permutations {
            let mut best = candidates[permutation[0]].clone();
            for &index in &permutation[1..] {
                if candidates[index].is_better_than(&best) {
                    best = candidates[index].clone();
                }
            }
            assert_eq!(best.placed_area, candidates[2].placed_area);
        }
    }

    #[test]
    fn metric_buckets_are_monotonic_across_ieee754_scales() {
        const MANTISSA_MASK: u64 = (1_u64 << 52) - 1;
        let mantissas = [
            0,
            1,
            METRIC_BUCKET_HALF - 1,
            METRIC_BUCKET_HALF,
            (1_u64 << METRIC_MANTISSA_DROP_BITS) - 1,
            1_u64 << METRIC_MANTISSA_DROP_BITS,
            1_u64 << 51,
            MANTISSA_MASK,
        ];
        let mut previous = 0;
        for exponent in 0_u64..=0x7fe {
            for mantissa in mantissas {
                let value = f64::from_bits((exponent << 52) | mantissa);
                let bucket = metric_bucket(value);
                assert!(
                    bucket >= previous,
                    "bucket ordering regressed at exponent {exponent:#x}, mantissa {mantissa:#x}"
                );
                previous = bucket;
            }
        }
        assert!(metric_bucket(f64::INFINITY) > previous);
    }

    fn synthetic_rank(placed_area: f64, stable_id: u64) -> CanonicalRank {
        CanonicalRank {
            valid: true,
            placed_area,
            placed_count: 1,
            occupied_area: 1.0,
            stable_key: vec![PlacementKey {
                placed: true,
                rotation_degrees: 0.0,
                translation_y: 0.0,
                translation_x: stable_id as f64,
                part_id: stable_id,
                exact_rotation_bits: 0.0_f64.to_bits(),
                exact_translation_y_bits: 0.0_f64.to_bits(),
                exact_translation_x_bits: (stable_id as f64).to_bits(),
            }],
        }
    }

    fn input_with_parts(parts: &[(u64, f64, f64)]) -> SolverInput {
        SolverInput {
            options: VacNestingOptions::default(),
            container: rectangle(0.0, 0.0, 1_000.0, 1_000.0),
            holes: Vec::new(),
            obstacles: Vec::new(),
            parts: parts
                .iter()
                .map(|(id, width, height)| Part {
                    id: *id,
                    components: vec![PartComponent {
                        outer: rectangle(0.0, 0.0, *width, *height),
                        holes: Vec::new(),
                    }],
                })
                .collect(),
        }
    }

    fn placements(input: &SolverInput, placed: &[(usize, f64, f64)]) -> Vec<VacNestingPlacement> {
        let mut results = input
            .parts
            .iter()
            .map(|part| VacNestingPlacement {
                part_id: part.id,
                ..VacNestingPlacement::default()
            })
            .collect::<Vec<_>>();
        for &(index, x, y) in placed {
            results[index] = VacNestingPlacement {
                part_id: input.parts[index].id,
                translation_x: x,
                translation_y: y,
                placed: 1,
                ..VacNestingPlacement::default()
            };
        }
        results
    }

    fn permute_input(input: &SolverInput, permutation: &[usize]) -> SolverInput {
        SolverInput {
            options: input.options,
            container: input.container.clone(),
            holes: input.holes.clone(),
            obstacles: input.obstacles.clone(),
            parts: permutation
                .iter()
                .map(|&index| input.parts[index].clone())
                .collect(),
        }
    }

    fn permute_results(
        results: &[VacNestingPlacement],
        permutation: &[usize],
    ) -> Vec<VacNestingPlacement> {
        permutation.iter().map(|&index| results[index]).collect()
    }

    fn rectangle(x: f64, y: f64, width: f64, height: f64) -> Polygon {
        Polygon(vec![
            VacNestingPoint { x, y },
            VacNestingPoint { x: x + width, y },
            VacNestingPoint {
                x: x + width,
                y: y + height,
            },
            VacNestingPoint { x, y: y + height },
        ])
    }
}
