# Benchmark results

One folder per measurement: `results.json` (the records, see [../README.md](../README.md) for the fields),
`machine.json`, and a `README.md` that says which VaM installation it was and gives the table. Numbers from different
folders are comparable only when the machine, the VaM installation, its preferences and the kit's preset are the same.

| Folder | Machine | VaM installation | Displays |
|---|---|---|---|
| [2026-11-11-rtx5090-repack-copy](2026-11-11-rtx5090-repack-copy/README.md) | RTX 5090 D, 16-core Ryzen X3D | content-free copy of a repacked VaM 1.22 (not an official installation) | desktop 4K, simulated headset 144 Hz |
| [2026-11-11-rtx5090-official-1.22.0.13](2026-11-11-rtx5090-official-1.22.0.13/README.md) | RTX 5090 D, 16-core Ryzen X3D | official VaM 1.22.0.13, new installation's preferences (soft body physics on), BepInEx 5.4.23.6 | desktop 4K, simulated headset 144 Hz |
