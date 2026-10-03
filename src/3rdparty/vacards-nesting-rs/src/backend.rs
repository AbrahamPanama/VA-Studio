// SPDX-License-Identifier: GPL-2.0-or-later

//! Internal optimizer backend boundary and monotonic incumbent ownership.
//!
//! The public job state machine remains the only C ABI publication path. A
//! backend can produce candidates internally, but only this canonical wrapper
//! may replace the retained complete solution.

use crate::job::{RunControl, SolverError, SolverInput, VacNestingPlacement};
use crate::rank::CanonicalRank;

pub(crate) trait NestingBackend {
    fn solve_backend(
        &self,
        input: &SolverInput,
        control: &RunControl<'_>,
    ) -> Result<Vec<VacNestingPlacement>, SolverError>;
}

#[derive(Clone, Debug)]
pub(crate) struct RankedSolution {
    placements: Vec<VacNestingPlacement>,
    rank: CanonicalRank,
}

impl RankedSolution {
    pub(crate) fn new(
        input: &SolverInput,
        placements: Vec<VacNestingPlacement>,
        geometry_valid: bool,
    ) -> Option<Self> {
        let rank = CanonicalRank::evaluate(input, &placements, geometry_valid);
        rank.is_valid().then_some(Self { placements, rank })
    }

    pub(crate) fn placements(&self) -> &[VacNestingPlacement] {
        &self.placements
    }

    pub(crate) fn rank(&self) -> &CanonicalRank {
        &self.rank
    }

    pub(crate) fn into_placements(self) -> Vec<VacNestingPlacement> {
        self.placements
    }
}

#[derive(Clone, Debug)]
pub(crate) struct CanonicalIncumbent {
    initial: RankedSolution,
    best: RankedSolution,
}

/// Small deterministic archive of feasible complete solutions. It retains the
/// strongest canonical ranks, deduplicates exact placement vectors, and never
/// exposes an infeasible or partial intermediate state.
#[derive(Clone, Debug)]
pub(crate) struct SolutionPool {
    capacity: usize,
    entries: Vec<RankedSolution>,
}

impl SolutionPool {
    pub(crate) fn new(capacity: usize) -> Self {
        Self {
            capacity,
            entries: Vec::with_capacity(capacity),
        }
    }

    /// Returns true only when `candidate` survives bounded pool eviction.
    pub(crate) fn insert(&mut self, candidate: RankedSolution) -> bool {
        if self.capacity == 0
            || self.entries.iter().any(|entry| {
                compare_placement_vectors(entry.placements(), candidate.placements())
                    == std::cmp::Ordering::Equal
            })
        {
            return false;
        }
        if self.entries.len() == self.capacity
            && self
                .entries
                .last()
                .is_some_and(|worst| !candidate.rank().is_better_than(worst.rank()))
        {
            return false;
        }

        let candidate_key = candidate.placements().to_vec();
        self.entries.push(candidate);
        self.entries.sort_by(|left, right| {
            right
                .rank()
                .quality_cmp(left.rank())
                .then_with(|| compare_placement_vectors(left.placements(), right.placements()))
        });
        self.entries.truncate(self.capacity);
        self.entries.iter().any(|entry| {
            compare_placement_vectors(entry.placements(), &candidate_key)
                == std::cmp::Ordering::Equal
        })
    }

    pub(crate) fn best(&self) -> Option<&RankedSolution> {
        self.entries.first()
    }

    pub(crate) fn entries(&self) -> impl Iterator<Item = &RankedSolution> {
        self.entries.iter()
    }

    #[cfg(test)]
    pub(crate) fn len(&self) -> usize {
        self.entries.len()
    }
}

fn compare_placement_vectors(
    left: &[VacNestingPlacement],
    right: &[VacNestingPlacement],
) -> std::cmp::Ordering {
    left.iter()
        .zip(right)
        .find_map(|(left, right)| {
            [
                left.part_id.cmp(&right.part_id),
                left.placed.cmp(&right.placed),
                left.rotation_degrees.total_cmp(&right.rotation_degrees),
                left.translation_y.total_cmp(&right.translation_y),
                left.translation_x.total_cmp(&right.translation_x),
                left.reserved.cmp(&right.reserved),
            ]
            .into_iter()
            .find(|ordering| ordering != &std::cmp::Ordering::Equal)
        })
        .unwrap_or_else(|| left.len().cmp(&right.len()))
}

impl CanonicalIncumbent {
    pub(crate) fn new(initial: RankedSolution) -> Self {
        Self {
            best: initial.clone(),
            initial,
        }
    }

    /// Atomically replaces the best internal solution only on strict canonical
    /// improvement. Discovery order cannot downgrade or perturb the incumbent.
    pub(crate) fn consider(&mut self, candidate: RankedSolution) -> bool {
        if candidate.rank.is_better_than(&self.best.rank) {
            self.best = candidate;
            true
        } else {
            false
        }
    }

    pub(crate) fn initial(&self) -> &RankedSolution {
        &self.initial
    }

    pub(crate) fn best(&self) -> &RankedSolution {
        &self.best
    }

    pub(crate) fn into_best(self) -> Vec<VacNestingPlacement> {
        debug_assert!(!self.initial.rank.is_better_than(&self.best.rank));
        self.best.into_placements()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::job::{Part, PartComponent, Polygon, VacNestingOptions, VacNestingPoint};

    #[test]
    fn malformed_or_geometry_invalid_candidates_never_enter_the_incumbent() {
        let input = sample_input();
        assert!(RankedSolution::new(&input, placements(&input, &[0]), true).is_some());
        assert!(RankedSolution::new(&input, Vec::new(), true).is_none());
        assert!(RankedSolution::new(&input, placements(&input, &[0]), false).is_none());
    }

    #[test]
    fn incumbent_never_regresses_and_preserves_the_initial_solution() {
        let input = sample_input();
        let initial_results = placements(&input, &[0]);
        let initial = RankedSolution::new(&input, initial_results.clone(), true).unwrap();
        let mut incumbent = CanonicalIncumbent::new(initial);

        let worse = RankedSolution::new(&input, placements(&input, &[]), true).unwrap();
        assert!(!incumbent.consider(worse));
        assert_eq!(incumbent.best().placements(), initial_results);

        let better = RankedSolution::new(&input, placements(&input, &[0, 1]), true).unwrap();
        assert!(incumbent.consider(better));
        assert_eq!(incumbent.initial().placements(), initial_results);
        assert_eq!(incumbent.best().rank().placed_count(), 2);

        let equal =
            RankedSolution::new(&input, incumbent.best().placements().to_vec(), true).unwrap();
        assert!(!incumbent.consider(equal));
    }

    #[test]
    fn canonical_reduction_is_independent_of_candidate_discovery_order() {
        let input = sample_input();
        let candidates = [
            placements(&input, &[0]),
            placements(&input, &[1]),
            placements(&input, &[0, 1]),
        ];
        let reduce = |order: &[usize]| {
            let initial = RankedSolution::new(&input, placements(&input, &[]), true).unwrap();
            let mut incumbent = CanonicalIncumbent::new(initial);
            for &index in order {
                incumbent.consider(
                    RankedSolution::new(&input, candidates[index].clone(), true).unwrap(),
                );
            }
            incumbent.into_best()
        };
        assert_eq!(reduce(&[0, 1, 2]), reduce(&[2, 1, 0]));
    }

    #[test]
    fn bounded_solution_pool_deduplicates_and_evicts_the_worst_rank() {
        let input = sample_input();
        let empty = RankedSolution::new(&input, placements(&input, &[]), true).unwrap();
        let one = RankedSolution::new(&input, placements(&input, &[0]), true).unwrap();
        let both = RankedSolution::new(&input, placements(&input, &[0, 1]), true).unwrap();
        let mut pool = SolutionPool::new(2);

        assert!(pool.insert(empty.clone()));
        assert!(!pool.insert(empty));
        assert!(pool.insert(one.clone()));
        assert!(pool.insert(both.clone()));
        assert_eq!(pool.len(), 2);
        assert_eq!(pool.best().unwrap().placements(), both.placements());
        assert!(
            pool.entries()
                .any(|candidate| candidate.placements() == one.placements())
        );
        assert!(
            !pool
                .entries()
                .any(|candidate| candidate.rank().placed_count() == 0)
        );
    }

    #[test]
    fn solution_pool_winner_is_permutation_stable() {
        let input = sample_input();
        let candidates = [
            RankedSolution::new(&input, placements(&input, &[]), true).unwrap(),
            RankedSolution::new(&input, placements(&input, &[0]), true).unwrap(),
            RankedSolution::new(&input, placements(&input, &[1]), true).unwrap(),
            RankedSolution::new(&input, placements(&input, &[0, 1]), true).unwrap(),
        ];
        let permutations = [
            [0, 1, 2, 3],
            [0, 2, 3, 1],
            [1, 3, 0, 2],
            [2, 0, 1, 3],
            [3, 2, 1, 0],
        ];

        let expected = candidates[3].placements();
        for permutation in permutations {
            let mut pool = SolutionPool::new(3);
            for index in permutation {
                pool.insert(candidates[index].clone());
            }
            assert_eq!(pool.best().unwrap().placements(), expected);
            assert_eq!(pool.len(), 3);
        }
    }

    #[test]
    fn zero_capacity_solution_pool_rejects_candidates() {
        let input = sample_input();
        let candidate = RankedSolution::new(&input, placements(&input, &[0]), true).unwrap();
        let mut pool = SolutionPool::new(0);
        assert!(!pool.insert(candidate));
        assert!(pool.best().is_none());
        assert_eq!(pool.len(), 0);
    }

    fn sample_input() -> SolverInput {
        SolverInput {
            options: VacNestingOptions::default(),
            container: rectangle(100.0, 100.0),
            holes: Vec::new(),
            obstacles: Vec::new(),
            parts: vec![
                Part {
                    id: 1,
                    components: vec![PartComponent {
                        outer: rectangle(20.0, 20.0),
                        holes: Vec::new(),
                    }],
                },
                Part {
                    id: 2,
                    components: vec![PartComponent {
                        outer: rectangle(10.0, 10.0),
                        holes: Vec::new(),
                    }],
                },
            ],
        }
    }

    fn placements(input: &SolverInput, placed: &[usize]) -> Vec<VacNestingPlacement> {
        input
            .parts
            .iter()
            .enumerate()
            .map(|(index, part)| VacNestingPlacement {
                part_id: part.id,
                translation_x: if placed.contains(&index) {
                    index as f64 * 25.0
                } else {
                    0.0
                },
                placed: u8::from(placed.contains(&index)),
                ..VacNestingPlacement::default()
            })
            .collect()
    }

    fn rectangle(width: f64, height: f64) -> Polygon {
        Polygon(vec![
            VacNestingPoint { x: 0.0, y: 0.0 },
            VacNestingPoint { x: width, y: 0.0 },
            VacNestingPoint {
                x: width,
                y: height,
            },
            VacNestingPoint { x: 0.0, y: height },
        ])
    }
}
