# Texture Scatter

Open **Object Editor → Texture Scatter** and check the group to activate it.

1. Click **Pick texture**, then click terrain to pick its strongest texture layer. **Ctrl + Alt + left-click** is the shortcut.
2. Select an M2 in the Asset Browser and click **Add copied M2s**. Repeat for a mix of models. Weights control their relative frequency. Existing M2s can also be copied before activating Scatter.
3. Drag left on terrain to paint the cyan selection. Ctrl-drag erases it. Choose **Circle** or **Square** under **Brush shape**; the cursor shows the matching footprint. **Alt + left-drag horizontally** changes the brush size, or use **Selection radius**. For a square, radius is half the side length. Resizing and picking do not paint the selection. **Clear area** removes the selection.
4. Set density, minimum spacing, minimum texture coverage, scale range, and rotation. Enable **Align to terrain** to tilt each model to the terrain triangle beneath its origin; the live preview updates automatically. It is off by default.
5. Preview M2s appear automatically as you paint or erase and update when placement settings change. **Preview** forces a refresh; **Reroll** changes the seed. **Place** becomes available once the latest preview is complete and commits those exact objects with one undo action for the batch. **Cancel preview** hides it until the next selection/settings change or explicit Preview.

Only points inside the painted selection whose picked texture is strongest and meets the coverage threshold are eligible. Holes and unloaded terrain are excluded. The selection uses cells approximately 1.04 world units wide. Texture checks use the terrain's local alpha texel, including unsaved paint edits.

Density is candidates per 100 square world units of eligible terrain. Minimum spacing can lower the final count. Spacing applies within this batch; it does not avoid existing objects. Placement tests the M2 origin, so branches and wider foliage can extend beyond texture or selection boundaries. Objects remain upright by default. With **Align to terrain**, the whole model rotates to the local slope and random heading rotates around that surface normal. This does not bend the mesh or fit a wide model to every bump beneath it. The completed preview and committed objects share the same full rotation, including Undo/Redo. Terrain height and slope are rechecked before committing.

There is no fixed selection-size limit: continue painting across terrain tiles. The filled cyan selection is 30% opaque so the underlying terrain remains visible. It is drawn in the terrain shader, and only changed visible mask tiles upload to the GPU. Selection painting does not raycast individual cells or rebuild line geometry. Scatter repaint requests set MapView's redraw flag before scheduling an update, including during held-button strokes. The placement limit remains 5,000 M2s per batch. Sampling and preview instance changes run in short slices, scheduled every 16 ms with a 4 ms work budget. Edits request a refresh after 180 ms; continuous painting is throttled rather than waiting for mouse release. Model loading yields until ready, and unchanged instances are reused. Cell-local candidate randomness avoids rerolling distant regions when extending the selection; spacing conflicts near edits can still change accepted neighbors. The 5,000-object cap uses stable randomized cell priority to distribute placements throughout the selection, instead of filling one world-coordinate region first. Extremely large selection snapshots and rendering complex M2s can still take time. Preview objects are not saved, selectable, or recorded in undo history. Changing settings or painting requests a new preview, keeping the previous objects visible while it is calculated. Place is disabled while updates are pending. Switching tools or navigating undo/redo clears and cancels the preview without automatically recreating it. Committing also stops automatic updates until the next edit, avoiding a duplicate preview on top of the placed batch. Switching tools retains the selected area and configuration; check the group again to resume. A changed or unloaded terrain/preview requires regeneration before placement.

## Verification

`tests/texture_scatter_sampling.cpp` exercises the same sampling implementation used by the tool: dominance and coverage, selection bounds, holes, ground height, minimum spacing across cell boundaries, weighted model choice, deterministic seeds, appearance changes without moving points, reroll, batch limits, density over eligible area, incremental sampling equivalence, input-order independence, and stable previews when distant cells are added.

`tests/scatter_selection.cpp` checks circle and square mask coverage across tile seams, matching erase footprints, clear, texture revision caching, repainting, map bounds, and continued painting beyond one million selected cells. It also reports brush CPU timing.

From a Visual Studio x64 developer shell in the source root:

```bat
cl /nologo /std:c++20 /EHsc /W4 /WX tests\texture_scatter_sampling.cpp /Fo:build\texture_scatter_sampling_test.obj /Fe:build\texture_scatter_sampling_test.exe
build\texture_scatter_sampling_test.exe
cl /nologo /std:c++20 /EHsc /O2 /W4 /WX tests\scatter_selection.cpp /Fo:build\scatter_selection_test.obj /Fe:build\scatter_selection_test.exe
build\scatter_selection_test.exe
```

In-editor acceptance check: select part of a grass/dirt pattern, preview a weighted M2 mix, verify no origins land on dirt or outside the cyan region, then Place → Undo → Redo. Save/reopen to verify ordinary placements persist, and save with an uncommitted preview to verify previews do not persist. Check Cancel preview, switching tools, and leaving the map while preview loading is active.

Live-preview acceptance check: hold the paint button while extending the region, erase it, and adjust density/spacing rapidly. Verify the final preview respects the latest settings and Place stays disabled until ready. Cancel during sampling/model loading, switch tools, undo/redo, and leave the map; no delayed preview should reappear. After Place, Ctrl+Z should remove the committed batch without creating another preview.

`tests/scatter_alignment.cpp` reconstructs the SceneObject YZX transform and checks upright, compound slopes, near-vertical slopes, multiple random headings, and degenerate-normal fallback. In the editor, compare aligned and upright grass on a hillside, then Place, Undo, Redo and save/reopen to check rotation persistence.

Crash regression: `tests/async_object_cache.cpp` uses the production shared-cache template with a controlled loader. It forces final-release/reacquire overlap for unfinished and finished-but-still-active loads. The old cache returned the retiring object and erased the replacement reference; the fixed cache detaches the retiring node under its lock and waits for the loader before destroying it. Loader failures also notify deletion waiters. This reproduces a cache lifetime defect, not the entire reported editor crash.

Cyan selection means the selected texture passes dominance and minimum coverage; orange means texture-rejected ground. This is an eligibility overlay, not a density guarantee. Preview attachments are checked periodically in short slices so returning to unloaded/reloaded terrain restores preview visibility.

**Sink depth** defaults to 0 and lowers model origins vertically by the specified world-unit distance (0�10, arrow increments 0.05). It updates the live preview without changing horizontal positions, spacing, scale or rotation. Try 0.20 for bushes. Place uses the exact lowered preview positions, and Undo/Redo preserves them. Terrain-change validation compares against the original sampled ground height, not the sunk origin. Check positive depth with Align to terrain both on and off; returning to zero should restore the original elevation.
