# Drift escape modes

When the main SPR-merge loop exhausts its patience budget without
improving the DAG score, `--drift N` triggers up to `N` drift-escape
attempts to find a new improving direction. `--drift-mode` selects
which mechanism runs per attempt.

## Modes

| Mode | Per-attempt behavior |
|------|----------------------|
| `legacy` | Sample tree, take 1–5 random neutral SPR steps, score all moves at radius `2*depth`, emit top `--max-moves` improving fragments, merge. |
| `inplace` | Sample tree, run `inplace_move_producer` for up to `--inplace-steps` steps under `--inplace-threshold` / `--inplace-temperature` / `--inplace-budget`; emit final state (or every step with `--inplace-fragments every`). Used automatically under `auto` when `--inplace-steps > 0`. |
| `combine` | Run the `inplace` prefix, then from the trajectory endpoint run the `legacy` fanout scoring. Merges both phases' fragments per attempt. |
| `combine-lwf` | Like `combine` but substitutes legacy's neutral-walk loop for the inplace prefix. Research baseline — strictly worse than `combine` on measured inputs (see below); not recommended for production. |
| `auto` (default) | `inplace` if `--inplace-steps > 0`, else `legacy`. Preserves pre-combiner dispatch. |

## When to use each

- **`legacy`** wins on easy plateaus where improving structure is
  dense within 1–5 neutral steps of the sampled tree. Low per-attempt
  cost, 50× fanout density per attempt.
- **`combine`** wins on hard plateaus where improving moves are
  several worsening-accepted steps away: its inplace walk reaches
  positions that a 1–5-step neutral walk cannot, and the fanout stage
  still captures dense-local structure. Also produces a substantially
  richer trimmed DAG (~8× more nodes in rotaA runs) because
  per-step fragments from the walk enlarge 629-region coverage.
- **`inplace`** alone is mostly useful for depth-reachable plateaus
  where the fanout stage is wasted overhead.

## Flags

| Flag | Default | Purpose |
|------|---------|---------|
| `--drift <N>` | off | Max drift attempts when patience triggers. |
| `--drift-mode <M>` | `auto` | `auto` \| `legacy` \| `inplace` \| `combine` \| `combine-lwf`. |
| `--drift-seed <N>` | `0xD1F75EED` | RNG seed consumed inside drift. Re-seeded at function entry so cross-mode comparisons are interpretable without per-run averaging. Decoupled from `--seed`. |
| `--inplace-steps <N>` | `0` | In-place SPR steps per attempt. `combine` defaults to 5 when this is unset. |
| `--inplace-threshold <T>` | `0` | Accept moves with `score_change <= T`. |
| `--inplace-budget <B>` | unlimited | Max cumulative worsening. |
| `--inplace-temperature <F>` | `0` | Simulated-annealing initial temperature. `0` is greedy. |
| `--inplace-cooling <F>` | `0.9` | Annealing cooling rate. |
| `--inplace-fragments <S>` | `final` | `every` or `final`. `every` emits per-step trajectory states as fragments. |

## Measurements — rotaA seed42/43/44 at pinned `--drift-seed 0xD1F75EED`

All six runs reach final parsimony **629**. Differentiating metrics:

| Seed | Mode | R3 att | Wall | Trimmed (nodes / edges) |
|:---:|:---|:---:|:---:|:---:|
| 42 | legacy  | 460 | 24:56 | 1 162 / 1 940 |
| 42 | combine | **173** | 28:55 | 10 307 / 23 008 |
| 43 | legacy  | 1   | 13:13 | 988 / 1 544 |
| 43 | combine | 1   | 23:24 | 7 828 / 17 234 |
| 44 | legacy  | 48  | 14:08 | 1 118 / 1 818 |
| 44 | combine | 68  | 25:06 | 8 830 / 19 594 |

Per-seed `combine / legacy` ratios:

| Seed | R3 hardness | R3 att | Wall | Trimmed nodes |
|:---:|:---|:---:|:---:|:---:|
| 42 | hard (460) | **0.38×** | 1.16× | 8.9× |
| 43 | trivial (1) | 1.00× | 1.77× | 7.9× |
| 44 | medium (48) | 1.42× | 1.78× | 7.9× |

Plateau hardness is the dominant variable. `combine` pays a per-attempt
overhead that dominates on easy plateaus, but its inplace walk's
broader reachable neighborhood cuts attempt count as plateaus get
harder — and the two effects cross on seed42's hard 630→629 plateau.
Trimmed-DAG enrichment is stable across hardness regimes.

## Examples

```sh
# Default (auto → legacy since --inplace-steps=0):
larch2 --dag-pb INPUT -n 500 --patience 5 --drift 500 \
       -o out.dag.pb.gz --trim

# Opt into the combiner:
larch2 --dag-pb INPUT -n 500 --patience 5 --drift 500 \
       --drift-mode combine \
       -o out.dag.pb.gz --trim

# Deeper prefix for hard plateaus:
larch2 --dag-pb INPUT -n 500 --patience 5 --drift 500 \
       --drift-mode combine \
       --inplace-steps 20 --inplace-temperature 0.8 \
       --inplace-fragments every \
       -o out.dag.pb.gz --trim

# Reproduce the rotaA measurement (at the default --drift-seed):
larch2 --dag-pb INPUT -n 500 --patience 5 --drift 500 \
       --drift-mode combine \
       --move-coeff-nodes 1 --move-coeff-pscore 1 \
       -o out.dag.pb.gz --trim
```

## Cross-drift-seed variance (N=4 per cell)

Sweep at 4 drift-seeds (`0xD1F75EED`, `1`, `42`, `0xBADDCAFE`) × 3
tree-seeds × 2 modes = 24 runs on rotaA.

- **Final parsimony 629: 24/24 runs.** Robust.
- **Trimmed-DAG enrichment: combine 6.9×–8.9× legacy** across 12
  (drift-seed, tree-seed) cells (median 7.9×). Stable.
- **Wall-clock combine / legacy: 1.05×–1.94× per cell** across drift-seeds.
- **Drift attempts to first reach 629 (seed42 hard plateau)**:
  combine is faster in 3/4 drift-seeds (0.14×, 0.38×, 0.38×) and
  slower in 1/4 (2.62×). Geometric mean 0.48× — directional
  support for combine's "broader-walk" advantage, but **high variance**;
  individual drift-seeds can produce combine advantage as extreme
  as 0.14× or disadvantage as extreme as 2.62×.
- On seed43/44, at 3 of 4 drift-seeds the 630→629 transition is
  handled by the **main SPR-merge loop** between drift rounds
  rather than inside a drift round; in those cases drift mode has
  negligible influence on the final escape.

Shippable claim: combine always reaches the same final parsimony as
legacy and produces a substantially richer trimmed DAG (~8×); its
attempt-count advantage on hard plateaus is real on average but
variable from drift-seed to drift-seed.

See `~/agent-memory/reports/s131-variance/RESULTS.md` for the full
matrix and methodology.

## Caveats

- N=1 at pinned drift-seed in the measurement section above; N=4
  per cell across the 4 drift-seeds listed under Cross-drift-seed
  variance. Attempt-count metrics have high variance across
  drift-seeds; final parsimony and trimmed-DAG enrichment do not.
- Measurement depth is rotaA-only. Other dataset families may
  differ. The theoretical argument (broader walk → fewer attempts
  on hard plateaus) is mode-intrinsic, not rotaA-specific.
