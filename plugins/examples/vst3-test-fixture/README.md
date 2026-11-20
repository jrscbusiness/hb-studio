# hb_vst3_test_fixture

**This is NOT a real/commercial VST3 plugin.** It is a minimal (fixed -6 dB gain, no UI, no
parameters) VST3 effect written for this project, built only to give `src/vst3/`'s tests something
real to load end to end — this environment has no license/way to obtain a genuine third-party VST3
plugin to test against. It is never installed, packaged, or shipped as part of HB Studio itself;
it exists solely as a CMake test-fixture target (`hb_vst3_test_fixture`, see
`cmake/modules/vst3.cmake`) consumed by `tests/vst3_scan_test.cpp` / `tests/vst3_effect_test.cpp`.

It deliberately has no `IPlugView`/editor (its `IEditController` is the SDK's own unmodified
`Steinberg::Vst::EditController`, whose `createView()` returns `nullptr`), so it cannot be used to
exercise `IVst3Editor` (src/vst3/windows/vst3_editor.cpp) — there is no way to get a real plugin
editor into this Linux-based environment to test that code path at all; see docs/VST3.md.
