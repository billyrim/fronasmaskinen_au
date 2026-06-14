# Fronasmaskinen AU v0.1

Minimal JUCE/CMake AU Instrument prototype for Logic Pro.

## Build

```bash
cmake -S . -B build -G Xcode
cmake --build build --config Debug
```

The AU target is configured with `COPY_PLUGIN_AFTER_BUILD TRUE`, so a successful build should copy
the component to the default user Audio Unit location for Logic validation.

## Prototype workflow

1. Open the AU Instrument in Logic.
2. Load a WAV or AIFF file.
3. Set `Start` and `End`.
4. Click `S1` to save the current selection.
5. Hold MIDI C1 in Logic to loop S1.
6. Adjust `Slot gain`, `Start trim`, and `End trim` for the selected slot.
