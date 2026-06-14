# Fronasmaskinen AU v0.1

Minimal JUCE/CMake AU Instrument prototype for Logic Pro.

## Build

```bash
cmake -S . -B build -G Xcode
cmake --build build --config Debug
```

The AU target is configured with `COPY_PLUGIN_AFTER_BUILD TRUE`, so a successful build should copy
the component to the default user Audio Unit location for Logic validation.

## Tests

```bash
cmake --build build --config Debug --target FronasmaskinenAUTests
./build/FronasmaskinenAUTests_artefacts/Debug/FronasmaskinenAUTests
```

The processor tests cover sample loading, waveform thumbnail generation, preview seek/play, two-click
loop creation, slot activation, trim/gain persistence, release, random start, mouse-style slot
auditioning, and MIDI triggering for S1-S10.

## Prototype workflow

1. Open the AU Instrument in Logic.
2. Load a WAV or AIFF file.
3. Set `Start` and `End`.
4. Click `S1` to save the current selection.
5. Hold MIDI C1 in Logic to loop S1.
6. Adjust `Slot gain`, `Start trim`, and `End trim` for the selected slot.
