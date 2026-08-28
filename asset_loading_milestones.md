# CPU Asset Loading and Data Integrity Milestones

Status: Approved implementation plan; implementation has not started.

Last updated: 2026-08-15

## Purpose

This document defines the staged redesign of Noggit Azure's CPU-side asset-loading system. It is the implementation and verification checklist for work involving file loading, parsing, asset identity, CPU-side caching, ownership, cancellation, ADT staging, and integration into live world data structures.

The motivating failure was an intermittent crash while loading adjacent ADTs. Investigation found that the crash is one symptom of broader problems in the current loading architecture: unstable resource lifetimes, immediate zero-reference eviction, repeated dependency acquisition, inconsistent failure states, unchecked parsing, and worker-thread mutation of live world structures.

The plan deliberately avoids solving only the observed race. Each milestone must improve the underlying loading and data-handling model while preserving existing world data.

## Scope

Included:

- File request scheduling and worker coordination.
- Safe, bounds-checked ADT, M2, WMO, and BLP reading.
- Asset-key normalization and request deduplication.
- Stable CPU asset records and explicit loading states.
- CPU-side caching, negative caching, eviction, and cancellation.
- ADT-level dependency deduplication.
- Parsing into private staging structures.
- Controlled integration of parsed data into live world structures.
- Thread-safe ownership and lifetime handling.
- Profiling queueing, I/O, parsing, allocation, caching, and contention.

Excluded:

- OpenGL uploads.
- GPU resource caches.
- Draw-time behavior.
- Render-context sharing of GPU resources.
- Frame upload budgets.
- Shaders and rendering changes.

The boundary of this work is:

```text
Request
  -> schedule
  -> read safely
  -> parse into staging data
  -> resolve/cache CPU dependencies
  -> commit CPU-side structures
  -> ready for consumer
```

## Baseline findings

The initial investigation established the following:

- Noggit starts three hard-coded async-loader workers.
- ADTs and terrain textures share the high-priority queue; models and WMOs use medium priority.
- MPQ reads are serialized by a global `ClientData` mutex.
- `MapTile::finished` means ADT parsing completed, not that dependencies completed.
- The existing `AsyncObjectMultimap` deletes entries immediately when reference count reaches zero.
- Its zero-reference deletion has an unlock/wait/relock window that permits reacquisition followed by deletion of the reacquired resource.
- Model and WMO handle copy/move assignment can leak manager references.
- Loader threads currently construct instances and participate in shared world mutation.
- `ClientFile` permits invalid seeks and contains unsafe bounds behavior after an out-of-range position.
- Failure handling differs by asset type. Some assets throw, some substitute data, and some can return without reaching a terminal state.
- Tracy support exists but is disabled and does not instrument the loading stages sufficiently.

Measured across `Azeroth_26_24.adt` through `Azeroth_26_28.adt`:

- 1,280 terrain chunks.
- 4,737 chunk-level terrain-texture references.
- 50 unique terrain textures.
- 2,170 M2 placement records.
- 2,100 unique M2 UIDs.
- 70 duplicate cross-tile M2 records.
- 23 WMO placement records representing 18 unique WMO UIDs.

This sample demonstrates that chunk-level ownership produces extensive manager traffic compared with the number of unique dependencies.

## Target architecture

### Checked file reader

Direct pointer arithmetic and unchecked seeks should be replaced by a reader over immutable bytes with operations equivalent to:

```cpp
class CheckedFileReader
{
public:
  expected<T, FileError> read<T>();
  expected<std::span<std::byte const>, FileError> readBytes(std::size_t count);
  expected<void, FileError> seek(std::size_t offset);
  expected<CheckedFileReader, FileError> subReader(std::size_t offset,
                                                   std::size_t size);
  expected<std::string_view, FileError> readCString(std::size_t max_length);
};
```

The reader must reject:

- offsets outside the file or enclosing chunk;
- overflow in offset and size arithmetic;
- truncated structures;
- unterminated strings;
- invalid chunk sizes;
- counts exceeding their enclosing chunks;
- invalid dependency, name, texture, and layer indices;
- more than four ADT terrain layers;
- invalid MCIN and MCNK offsets.

Errors must identify the asset, chunk type, byte offset, expected size, and available size.

### Stable asset registry

`AsyncObjectMultimap<T>` should be replaced by an `AssetRegistry<T>` with stable records and an explicit state machine:

```cpp
enum class AssetState
{
  Unloaded,
  Queued,
  Reading,
  Parsing,
  Ready,
  Failed,
  Cancelled
};

template<typename T>
struct AssetRecord
{
  AssetKey key;
  std::atomic<AssetState> state;
  std::shared_ptr<T const> data;
  std::optional<AssetError> error;
  std::shared_future<AssetResult<T>> completion;
  std::size_t resident_bytes;
  std::uint64_t last_access;
};
```

Required semantics:

- One record per normalized CPU asset key.
- Concurrent requests share one physical load.
- Record addresses remain stable while handles or work reference them.
- Consumers hold handles rather than manager-owned raw pointers.
- Failed assets are negatively cached.
- Cache policy controls removal; a zero-reference transition does not immediately erase a record.
- State transitions are monotonic within a load generation.
- Every load generation reaches exactly one terminal state.

### Canonical asset identity

`AssetKey` should include:

- normalized lowercase internal path or FileDataID;
- asset type;
- client/project source generation.

CPU asset identity must not contain render context. Slash variants, case variants, and `.mdx`/`.mdl`/`.m2` aliases must canonicalize before lookup.

### Separation of work

Worker threads may:

- read immutable file bytes;
- validate formats;
- parse immutable CPU structures;
- request CPU dependencies;
- produce staging results.

Worker threads must not:

- mutate `World` or `MapIndex`;
- add or remove stored instances;
- modify selection or action history;
- attach objects to live tiles;
- retain pointers into temporary file buffers.

### ADT staging

ADT parsing should produce an owning result similar to:

```cpp
struct AdtLoadResult
{
  TileIndex index;
  AdtHeader header;
  std::array<ChunkData, 256> chunks;

  std::vector<AssetKey> terrain_textures;
  std::vector<ModelPlacementData> m2_placements;
  std::vector<WmoPlacementData> wmo_placements;

  std::vector<AssetHandle<ModelData>> models;
  std::vector<AssetHandle<WmoData>> wmos;
  std::vector<LoadWarning> warnings;
};
```

All staging fields must own their data. Chunk texture layers should use compact tile-local dependency indices rather than independently acquiring the same global texture record.

### Controlled integration

A completed staging result must be submitted to a world-integration queue. Integration initially runs on the main/world thread and must:

1. Verify request and source generations.
2. Validate staging invariants.
3. Resolve duplicate placement UIDs.
4. Construct final tile and instance structures.
5. Build tile/object relationships.
6. Publish the CPU-ready tile only after all checks succeed.

Integration failure must leave the previous world state unchanged.

### CPU cache policy

The initial cache should use:

- a configurable memory budget;
- LRU ordering;
- a minimum residency/grace period;
- pinned records while loading or integrating;
- retained negative-cache entries;
- source-generation invalidation.

Eviction is allowed only when a record has no active handles, pending load, or integration dependency.

### Cancellation

Each tile request receives a request generation or token. Cancellation should remove queued work where possible and prevent obsolete staging results from being committed. Cancelling one consumer must not cancel a shared request still required by another consumer.

## Milestone 0: Baseline instrumentation

### Implementation

Add measurements without changing loader behavior around:

- async queue entry, dequeue, and completion;
- `ClientData::readFile` and external-file reads;
- ADT, M2, WMO, and BLP loading;
- asset-manager acquisition and release;
- world-instance insertion;
- ADT dependency discovery.

Record:

- queue wait and execution time;
- bytes read;
- read and parse time separately;
- manager-lock wait time;
- acquisition count by normalized key;
- first, repeated, and concurrent requests;
- success, failure, and repeated failure counts;
- total versus unique dependencies per ADT;
- peak queue depth;
- live and zero-reference record counts.

### Testing and consistency rules

- Release behavior and asset results must remain unchanged.
- Profiling must be disabled or negligible in normal builds.
- The same reproduction route must produce the same tile, object, texture, and placement counts.
- Profiling callbacks must not acquire loader, registry, or world locks.

### Profiling procedure

Capture at least three runs of each scenario:

1. Load directly into the existing map position.
2. Move through the `Azeroth_26_24` through `26_28` region.
3. Move far enough to unload the region and then return.

Retain median and worst-case results.

### Exit criteria

- Queueing, I/O, parsing, registry, and integration time are distinguishable.
- Repeated loads and manager contention are quantified.
- Results identify individual assets and ADTs.

## Milestone 1: Safe file-reading foundation

### Implementation

Implement `CheckedFileReader` and migrate readers in this order:

1. ADT outer chunks and MHDR/MCIN.
2. MCNK and terrain layer structures.
3. MDDF and MODF placements.
4. M2 headers and indexed arrays.
5. WMO root and group chunks.
6. BLP headers and mip ranges.

Save writers are not migrated in this milestone.

### Data-consistency rules

- Every read is bounded by file size and enclosing chunk size.
- Arithmetic is checked for overflow.
- Every index is validated before use.
- Counts have explicit practical limits.
- Strings terminate inside their declared block.
- Failed parsing exposes no partial public object.
- Identical bytes produce identical data or an identical structured error.

### Testing

Add fixtures for:

- representative good files;
- truncation at multiple boundaries;
- oversized chunks;
- invalid MCIN and MCNK offsets;
- invalid MDDF and MODF name indices;
- invalid texture indices;
- more than four terrain layers;
- unterminated strings;
- overflowing offsets and counts;
- empty and missing files.

Tests assert the structured error and offset, not only that an exception occurred. Run malformed-input tests under AddressSanitizer where supported or Application Verifier/PageHeap otherwise.

### Profiling

Compare file-read time, parse time, allocation count, bytes copied, and peak temporary memory against Milestone 0. Normal validation overhead should remain near or below 5% of CPU parse time.

### Exit criteria

- Malformed fixtures cannot cause access violations or unbounded allocation.
- `Azeroth_26_26.adt` parses successfully or returns an exact bounded error.
- Good-file parsed counts match the old parser.
- Release build, deployment, and hash verification pass.

## Milestone 2: Asset registry and explicit state machine

### Implementation

Implement `AssetRegistry<T>` for M2 models first. Leave ADTs, WMOs, and BLPs on the old manager temporarily.

### Required semantics

- Concurrent requests for one key create one record and one load.
- All requesters receive the same stable record.
- Loading or referenced records cannot be erased.
- Failure completes every waiter exactly once.
- Waiting cannot hang after success, failure, or cancellation.
- Handle copy and move operations preserve correct ownership.
- Raw pointers cannot outlive records.

### Testing

Use deterministic barriers, not sleeps, to test:

- 100 simultaneous requests for one asset;
- final-handle release during loading;
- reacquisition during the old deletion window;
- failure with multiple waiters;
- cancellation by one of several consumers;
- repeated handle copy, move, and assignment;
- registry destruction with queued and completed records.

Use ThreadSanitizer where supported; otherwise use repeated stress runs with Application Verifier.

### Profiling

Measure registry-lock wait, lookup time, coalesced requests, record allocations, physical loads per key, retained memory, and repeated failure suppression.

### Exit criteria

- One physical M2 load occurs per key and source generation.
- Incompatible assets such as `spells/blanket.m2` are not repeatedly parsed in one session.
- Concurrency tests pass repeatedly.
- No stale raw pointers remain in the migrated M2 path.
- Placement and model counts match baseline.

## Milestone 3: WMO and BLP registry migration

### Implementation

Move WMO and BLP CPU data to the new registry and unify failure semantics.

Required behavior:

- Missing WMOs reach `Failed` rather than returning unfinished.
- Missing texture fallback reaches a stable terminal record.
- Incompatible M2/WMO data is negatively cached.
- All waiters observe the same result.

### Data-consistency rules

- WMO child dependencies use stable handles.
- A failed child cannot leave a parent permanently pending.
- Fallback identity remains distinct from the requested missing identity.
- Failure records preserve the requested path and cause.
- CPU data is not duplicated for different consumers solely because their render contexts differ.

### Testing

- Missing root WMO.
- Missing WMO group.
- Missing WMO doodad M2.
- Missing BLP with fallback.
- Invalid BLP compression.
- Invalid mip offset and size.
- WMO with repeated dependencies.
- Multiple simultaneous ADTs requesting one WMO.

### Profiling

Measure unique versus physical loads by type, failure-cache hits, dependency fan-out, MPQ lock wait, memory by asset type, and cache-hit latency.

### Exit criteria

- No asset remains indefinitely between queued and terminal states.
- Missing assets are read and diagnosed once per source generation.
- CPU duplication caused solely by consumer context is removed.
- Successful asset counts match baseline.

## Milestone 4: ADT staging and dependency deduplication

### Implementation

Convert ADT parsing to produce `AdtLoadResult`. Worker parsing must not integrate into `World`.

### Data-consistency rules

- Staging owns all chunk and placement data.
- A normal ADT contains exactly 256 validated chunks.
- Chunk layers reference the ADT dependency table.
- Placements retain original UID and source ADT.
- UID correction does not occur during parsing.
- Any fatal parse error discards the complete staging result.
- Staging records source identity and generation.

### Testing

Compare old and staging parsers for representative ADTs:

- flags and coordinates;
- 256 chunk positions;
- terrain heights and normals;
- holes and area IDs;
- texture paths, layer flags, and alphamap checksums;
- water layer counts and checksums;
- M2 and WMO placement records;
- flight bounds.

Large arrays may use deterministic checksums; structural fields require exact comparison.

### Profiling

Measure total parse time, total and unique dependencies, registry requests, allocation count, peak staging memory, path-normalization time, and lock wait.

For the five-tile baseline, global terrain dependency acquisition should move from roughly 4,737 chunk acquisitions toward roughly 50 unique acquisitions plus inexpensive tile-local indexing.

### Exit criteria

- Staging matches the old parser for good fixtures.
- Worker parsing performs no `World` or `MapIndex` mutation.
- Invalid ADTs leave no partial tiles or instances.
- Dependency-manager traffic drops substantially.

## Milestone 5: Controlled world integration

### Implementation

Introduce a single ADT integration queue, initially executed on the main/world thread.

Integration steps:

1. Confirm request and source generations.
2. Validate staging invariants.
3. Resolve placement UID duplicates.
4. Construct final CPU tile structures.
5. Insert or reference M2/WMO instances.
6. Build tile/object relationships.
7. Publish the completed tile.
8. Release staging memory.

Publication is the final step.

### Data-consistency rules

Before publication:

- consumers cannot retrieve the new tile;
- placements are not visible in world storage;
- the previous tile remains unchanged.

After publication:

- every tile UID resolves to one stored instance;
- every referenced instance has correct tile backreferences;
- per-UID counts match ADT records;
- one logical object is not stored twice;
- tile coordinates agree with `MapIndex`;
- failed integration preserves the prior tile.

### Testing

- Integrate one tile.
- Integrate adjacent tiles with overlapping UIDs.
- Submit the same result twice.
- Cancel before and while queued for integration.
- Reload an existing tile.
- Force UID-resolution failure.
- Destroy the world with integration queued.
- Unload immediately after integration.

Add a debug consistency validator covering `MapIndex`, tile UID lists, object storage, object-to-tile backreferences, and per-UID counts. Run it after integration and unload operations in test builds.

### Profiling

Measure integration queue wait, integration duration, UID lookup time, allocations, world-lock duration, and staging memory release.

### Exit criteria

- Loader workers no longer mutate live world structures.
- The validator passes after load, reload, cancellation, and unload.
- Failed or cancelled staging leaves no residue.
- Object and tile counts match baseline.

## Milestone 6: CPU cache, eviction, and cancellation

### Implementation

Implement bounded retention after all CPU asset types use stable records.

Initial policy:

- configurable total CPU budget;
- accounting by ADT, M2, WMO, and BLP;
- minimum residency after last access;
- LRU eviction of unreferenced terminal records;
- retained failed records;
- loading and integrating records pinned against eviction;
- cancellation checks before read, before parse, and before integration.

### Data-consistency rules

- Eviction changes residency, not asset identity.
- Active handles remain valid.
- One consumer's cancellation does not fail a request shared by others.
- Stale generations cannot publish into the current world.
- Memory accounting equals the sum of resident record sizes.

### Testing

- Tiny budgets forcing continuous eviction.
- Reacquisition before and after eviction.
- Simultaneous lookup and eviction.
- Failed-record eviction.
- Project-file replacement and source invalidation.
- Rapid demand changes and cancellation storms.
- World destruction with queued/cancelled work.

### Profiling

Measure resident memory, hit/miss rate, eviction and reload counts, cancellation stage, bytes read for never-integrated assets, negative-cache savings, and away-and-return behavior versus Milestone 0.

### Exit criteria

- Memory stays within the configured budget plus documented pinned overhead.
- Recently visited regions achieve a high cache-hit rate.
- Cancellation materially reduces unused parsing.
- No stale integrations, dangling handles, or use-after-free failures occur.

## Global milestone gates

Every milestone must:

1. Preserve unrelated user-owned changes.
2. Keep changes limited to the milestone's stated scope.
3. Run all new parser, concurrency, and consistency tests.
4. Run `git diff --check` on edited files.
5. Build the Release `noggit` target successfully.
6. Deploy to `Noggit-Azure-Build/Noggit-Azure.exe`.
7. Confirm compiler-output and deployed SHA-256 hashes match.
8. Re-run the same map-loading reproduction.
9. Compare structural asset counts with the prior milestone.
10. Capture before-and-after profiling results.
11. Document any intentional data-count difference.
12. Stop the milestone if unexplained terrain, texture-layer, water, placement, UID, or tile-reference differences appear.

Performance improvements are not accepted if they introduce nondeterministic parsing, incomplete terminal states, stale publication, or unexplained world-data changes.

## Milestone progress

- [ ] Milestone 0: Baseline instrumentation
- [ ] Milestone 1: Safe file-reading foundation
- [ ] Milestone 2: M2 asset registry and state machine
- [ ] Milestone 3: WMO and BLP registry migration
- [ ] Milestone 4: ADT staging and dependency deduplication
- [ ] Milestone 5: Controlled world integration
- [ ] Milestone 6: CPU cache, eviction, and cancellation
