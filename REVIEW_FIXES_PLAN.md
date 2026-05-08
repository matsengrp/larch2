# Thrifty / NN-Sampling Review Fixes — Implementation Plan

Status as of branch tip `d405d1c` (claude/review-thrifty-sampling-pwmxp).

Each phase below is sized as one logical commit. Phases are ordered so earlier
work doesn't block later work, and so high-confidence safety fixes land first.
Out-of-scope items (not part of the thrifty sampling feature) are listed at the
end and should be split off into separate PRs.

For each item, the original review label (B# / I# / NIT) is given for cross-
reference. "Triage" reflects findings from re-reading the current code: REAL =
issue is present, OVERSTATED = the framing is wrong but a related action is
still useful, DEFANGED = the new strict parser / validation already neutralizes
most of the impact.

## Phase 1 — Critical safety

Goal: close the two items where the program can crash or stall in production.

### 1.1 Vulkan dispatch RAII (B5)

- File: `src/vulkan_compute.cpp:411-451`.
- Wrap `cmd` (`VkCommandBuffer`) and `fence` in scope-guard types so any throw
  between `vkAllocateCommandBuffers` and the cleanup at lines 450-451 frees
  both. Two small RAII structs (one per resource), or one combined
  `dispatch_resources` struct with destructor calling `vkDestroyFence` and
  `vkFreeCommandBuffers`.
- Test: add a unit test that injects a synthetic failure (e.g. by submitting
  a too-large work group) and asserts the pool counter is unchanged. If
  injection is impractical, add a comment explaining why and rely on review.
- Risk: low. Existing happy-path semantics are preserved.

### 1.2 Vulkan tests skip cleanly without device (B7)

- Files: `test/vulkan_compute_test.cpp`, `test/nn_inference_test.cpp`,
  `.github/workflows/build.yml:49`.
- Wrap `vk_context ctx;` construction (and the rest of `main`) in
  `try { ... } catch (std::exception const& e) { std::cerr << "skipping: " << e.what() << "\n"; return 77; }`.
  CTest treats exit 77 as SKIPPED.
- Set the skip code in `CMakeLists.txt`:
  `set_tests_properties(vulkan_compute_test nn_inference_test PROPERTIES SKIP_RETURN_CODE 77)`.
- Once the runtime guard is in place, drop the
  `'vulkan_compute_test|nn_inference_test'` exclusion regex in
  `.github/workflows/build.yml:49` so CI without Vulkan still runs (and skips)
  these binaries.
- Risk: low.

## Phase 2 — NN core: dedup and correctness

Goal: reduce duplication that will silently rot, plus two test-fix oversights.

### 2.1 Single source of `forward_result` and `log_likelihood` (I1)

- Files: `include/larch/rs_fivemer_model.hpp:29-32, 110-117`,
  `include/larch/indep_rs_cnn_model.hpp:117-120, 248-255`,
  `include/larch/nn_inference.hpp:19-22`, `src/nn_inference.cpp:266-272`.
- Lift `forward_result` to a single header (likely `include/larch/likelihood.hpp`
  alongside `poisson_context_log_likelihood`).
- Lift the `log_likelihood(parent_seq, child_seq)` body to a free template/
  function that takes a `forward()`-callable model. Each model retains its own
  `forward()` overload (CPU vs Vulkan vs CNN) but defers to one
  `compute_log_likelihood(forward_result const&, parent_seq, child_seq)`.
- Risk: medium. Touch all three call sites; existing test coverage in
  `nn_s5f_test`, `nn_cnn_test`, `nn_inference_test` should catch regressions.

### 2.2 Null-check pimpl in `nn_inference` public methods (I3)

- File: `src/nn_inference.cpp:261, 263, 274`.
- Two reasonable options; pick one:
  1. **Recommended**: `= delete` move ctor + move assignment. `nn_inference` is
     not currently moved across object boundaries in production code; deleting
     moves makes the API non-movable and the issue goes away.
  2. Keep movability and guard each public method with
     `if (!impl_) throw std::logic_error{"nn_inference: moved-from object"};`.
- If option 1: also remove the now-unnecessary `unique_ptr<impl> impl_` defaulting.
- Risk: low.

### 2.3 Fix `test_different_contexts_different_rates` (I11)

- File: `test/rs_fivemer_model_test.cpp:184-196`.
- Add `assert(rates1[4] != rates2[4])` (or a nontrivial relative-difference
  bound) to actually verify the property the test name promises.
- Risk: trivial. If the assertion fails on the bundled `s5f` model, the test
  was vacuously passing before; investigate.

### 2.4 Tighten or document `nn_s5f_test` 20% tolerance (I9)

- File: `test/nn_s5f_test.cpp:239-249`.
- The 20% bound is on a log-likelihood, not on a probability. Two options:
  1. Run the test once on the bundled `s5f` model, record the worst observed
     relative error per position, and pin the tolerance just above that
     (e.g. 1.5×). Document the source.
  2. Split the assertion: a tight bound (e.g. 1e-4) on the dominant terms,
     and a separate softmax-underflow check that explicitly tolerates positions
     where `csp[child_base] < 1e-30` was clamped.
- Risk: low. The current bound passes; the goal is preventing a regression
  that the current bound would silently allow.

## Phase 3 — Loader robustness

Goal: untrusted-input hardening for the model-load path.

### 3.1 Pickle VM growth caps (B2)

- File: `src/pickle_reader.cpp:105-110` (state) plus the op handlers.
- Add `inline constexpr` caps near the top:
  `k_pickle_max_stack = 10000`, `k_pickle_max_memo = 1'000'000`,
  `k_pickle_max_tensors = 10'000`. Numbers are tunable; pick what's clearly
  larger than any real PyTorch checkpoint we'd accept.
- After each push to `stack_`, `memo_`, or `tensors_`, check the cap and throw
  `std::runtime_error{"pickle_reader: <name> grew past limit"}` on breach.
- Risk: low. Caps must be larger than realistic models — confirm by running
  against the bundled `s5f.pth` and `ThriftyHumV0.2-45-libtorch.pth`.

### 3.2 `std::byteswap` instead of `__builtin_bswap64` (B3)

- File: `src/pickle_reader.cpp:156`.
- Replace `__builtin_bswap64(bits)` with `std::byteswap(bits)`. Add
  `#include <bit>`. C++26 is required by the project so this is available.
- Risk: trivial. Note: the original review's claim about silent miscompile
  is incorrect; missing builtin is a compile error, not silent wrong values.

### 3.3 Explicit YAML scalar/map distinction (I2)

- File: `include/larch/yaml_reader.hpp:18-23`.
- Today: `bool is_map() const { return scalar.empty(); }` — a value parsed as
  `key:` (empty value) is misclassified as a map.
- Replace with an explicit discriminator: add `bool has_value_ = false;` to
  `yaml_value`, set it when a scalar is assigned, and define
  `bool is_scalar() const { return has_value_; }` and
  `bool is_map() const { return !has_value_ && !map.empty(); }`.
- Update `as_string()` and `as_int()` / `as_float()` to throw with a clear
  message when the value is empty rather than silently returning `""` / 0.
- Risk: low. The new strict parser already rejects most malformed inputs;
  the remaining hole is the `key:` case.

### 3.4 Debug-build kmer index assertion (B1, downgraded)

- Files: `include/larch/rs_fivemer_model.hpp:91-103`,
  `include/larch/indep_rs_cnn_model.hpp:57-63` (`embedding(...)`).
- Constructor shape checks already prevent OOB under matched configurations,
  so this is defense-in-depth, not a real-bug fix.
- Add `assert(idx < encoder_.kmer_count())` at each indexing site. No release
  cost; catches future regressions.
- Risk: trivial.

## Phase 4 — API surface tightening

Goal: type-safety improvements that prevent classes of future bugs.

### 4.1 Tighten `ml_scoring_config::model` to `const*` (B4, downgraded)

- File: `include/larch/model_variant.hpp:181`.
- Change `ml_model* model = nullptr;` to `ml_model const* model = nullptr;`.
  Update every assignment site (`tools/larch2.cpp` ~`ml_config.model = &*ml_model_storage;`
  is already taking the address of an `ml_model` value — should compile).
- Note: the original review's "any move invalidates every tensor span" is
  wrong; mmap pointers survive `pth_file` move because the `zip_reader`
  RAII moves with the model. The `const*` change is a hygiene fix only.
- Risk: low. Catches accidental mutation through the config in callers.

### 4.2 Implement or remove `vk_buffer::usage` (B6)

- Files: `include/larch/vulkan_compute.hpp:37-39`,
  `src/vulkan_compute.cpp:200-211`, plus call sites in
  `src/nn_inference.cpp:111-117, 163-169`.
- Pick one:
  1. **Recommended for now**: remove the enum entirely; constructors become
     `vk_buffer::create(ctx, byte_size)`. All current uses are storage-buffer
     reads/writes inside the same dispatch.
  2. Implement: translate `storage_read` → add `VK_BUFFER_USAGE_TRANSFER_SRC_BIT`,
     etc. Useful only if we ever stage buffers across queues.
- Risk: low for option 1 (mechanical removal); option 2 needs more thought.

### 4.3 Replace stringly-typed `sample_method` with `enum class` (I4)

- Files: `include/larch/sample_method.hpp` (new enum + parse helpers),
  `tools/larch2.cpp`, `tools/dagutil.cpp`, `include/larch/random_optimize.hpp:39`.
- Define `enum class sample_method { parsimony, random, rf_minsum, rf_maxsum,
  ml, edge_weight }`. Parse from string at the CLI boundary; pass the enum
  internally. Keep `is_ml_sample_method` etc. as `constexpr` enum predicates.
- For `objective_kind` in `optimize_result`: introduce a parallel enum (or
  reuse `sample_method` if every kind maps 1-1 today).
- Risk: medium — touches both CLIs and the CMake test name patterns. Do it as
  a separate commit so it can be reverted if unforeseen issues arise.

## Phase 5 — CLI consolidation

Goal: remove duplicated load logic and dead branches.

### 5.1 Extract `build_ml_config` in larch2 (I6)

- File: `tools/larch2.cpp:1366-1390` (run_native) and `1833-1851` (run_random).
- Extract a helper:
  ```
  std::pair<std::optional<ml_model>, ml_scoring_config>
  build_ml_config(args const&);
  ```
  that loads the model when needed and populates `ml_scoring_config`. Both
  call sites become a few lines.
- Remove the `if (!has_ml) { ... exit(1); }` guard inside the helper —
  `validate_args:357` already enforces this for ml-sampling, and lines 369-372
  enforce it for `--move-coeff-ml > 0`. After validation, `needs_ml_model`
  implies `has_ml`. Confirmed dead code.
- Risk: low.

### 5.2 Single `load_ml_model` in dagutil main (I7)

- File: `tools/dagutil.cpp:559-565` and `707-713`.
- `validate_args:385-389` makes `--edge-ml` and `--sample` mutually exclusive.
  Move the load above both branches:
  ```
  std::optional<ml_model> ml_model_storage;
  if (a.edge_ml || is_ml_sample_method(a.sample_method))
    ml_model_storage = load_ml_model(a.model_dir, a.model_name);
  ```
- Risk: low.

### 5.3 Paired-or-neither model-arg check in dagutil (I8)

- File: `tools/dagutil.cpp` `parse_args` validation block (~line 347).
- Add the same check larch2 has:
  ```
  if ((!a.model_dir.empty()) != (!a.model_name.empty())) {
    std::cerr << "error: --model-dir and --model-name must be provided together\n";
    std::exit(1);
  }
  ```
- Risk: trivial.

## Phase 6 — Cleanup and documentation

Goal: NIT bucket. Each item is small; can be a single commit or split.

- **`shaders/double_it.comp`** (`CMakeLists.txt:120`). Move to a test-only
  shader rule so it's not in the production binary's resource list. Either
  rename + gate, or define a separate `compile_shaders(... TARGET larch_test_shaders)`
  call inside `if(BUILD_TESTING)`.
- **Tie-tolerance asymmetry doc**. Add a one-line comment to
  `include/larch/weight_ops.hpp` (parsimony ops `within_clade_accum`) and
  `include/larch/likelihood_score_ops.hpp:163-170` explaining why integer
  weights use exact `==` while doubles use `within_min_weight_tie`.
- **Magic Vulkan binding-slot constants** (`src/nn_inference.cpp:107, 154, 156`).
  Promote `6`, `12`, `11` to `inline constexpr std::size_t k_*_binding_count`
  near the shader-bridge code.
- **Field name `sc`** (`src/nn_inference.cpp:58`). Rename to `site_count_`.
- **Inline `scored_frag` struct** (`tools/larch2.cpp:1532, 1554, 1704, 1725`).
  Define once at file scope; reuse.
- **`--edge-thrifty` alias visibility** (`tools/dagutil.cpp:189-207`,
  `tools/dagutil.cpp:304`, `README.md:262`). Either add a line to `usage()`
  mentioning the alias, or drop it entirely. Recommend dropping unless it's
  documented somewhere users follow.
- **`test/test_util.hpp:69`**. Replace `assert(false ...); return {};` with
  `std::unreachable();` (`#include <utility>`, C++23).
- **`test/expect_fail_regex.sh:21`**. Add a diagnostic on no-match:
  ```sh
  if ! printf '%s\n' "$output" | grep -Eq "$regex"; then
    echo "output did not match regex '$regex'" >&2
    exit 1
  fi
  ```
- **`make_test_tree()` / `cg_from_sequence()` duplication**. Lift to
  `test/test_util.hpp` from `nn_ml_spr_test.cpp` and `nn_blended_spr_test.cpp`.
  Watch for the small test-fixture variants (e.g. `make_ua_edge_test_tree`)
  and parametrize.

## Out of scope for this PR

These are real but unrelated to the thrifty sampling feature; split off:

- **B8** `--tree-suffix` filter bug in `tools/larch2-bcr.cpp:125-129`.
  Separate CLI tool.
- **I5** O(N²) Newick edge wiring in `include/larch/build_fasta_newick.hpp:78-105`.
  Input loader, predates this branch's NN work.
- **I10** Hardcoded `/home/matsen/...` path in `test/merge_consistency_test.cpp:233-242`.
  Adjacent merge-invariant test.
- **Dockerfile NIT** (`Dockerfile:33` `LD_LIBRARY_PATH` on ARM64). Build infra.

## Items intentionally not addressed

- **B1 OOB read** is not a real bug under the existing constructor shape
  checks; only the debug assertion in 3.4 is worth doing.
- **B3 silent miscompile** framing is wrong — addressed via the `std::byteswap`
  swap in 3.2 but not for the reason claimed.
- **B4 move-invalidates-spans** framing is wrong — the `const*` tightening in
  4.1 is good hygiene, but the underlying lifetime model is sound.

## Suggested order for landing

1. Phase 1 (safety) — ~1 commit.
2. Phase 3 (loader hardening) — ~1 commit, can land in parallel with Phase 1.
3. Phase 2 (NN core dedup) — ~2 commits (split I1 from the test fixes).
4. Phase 5 (CLI consolidation) — 1 commit.
5. Phase 4 (API tightening) — 1-2 commits; defer 4.3 (enum) if it grows.
6. Phase 6 (cleanup) — 1 commit, batched.

Out-of-scope items (B8, I5, I10) get their own PRs.
