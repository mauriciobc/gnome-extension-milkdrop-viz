SDL snapshot baselines (P6 PPM, 320x240, frame 48, MILKDROP_FIXED_FRAME_TIME=1)
================================================================================

Subfolder: sdl_seed42_f48_w320h240/

  baseline_01.ppm … baseline_05.ppm — rendered with build/tests/sdl_preset_snapshot
  MANIFEST.txt — tab-separated: baseline filename <TAB> absolute path to source .milk

Regenerate (from repo root, needs DISPLAY):

  bash tools/create_sdl_snapshot_baselines.sh

Defaults: scan /home/mauriciobc/presets if it exists, else reference_codebases/projectm/presets/tests.
Override: MILKDROP_COMPARE_PRESET_DIR, MILKDROP_COMPARE_RANDOM_SEED, MILKDROP_COMPARE_RANDOM_COUNT.

Use with GTK parity checks: for each line in MANIFEST, render the same .milk with
reference_renderer and compare_projectm_snapshots.py against the matching baseline_*.ppm.
