## Neural Amp Modeler captures

`.nam` captures are read by `nts_ml.nam` and converted to packed **NTSM v3**, which extends the
packed model format with a WaveNet architecture code (5) and a per-layer `{kernelSize, dilation}`
table following the header — WaveNet geometry cannot be expressed in the fixed v1/v2 headers.
Versions 1 and 2 parse exactly as before.

Supported: NAM format 0.7.0, `WaveNet` and `SlimmableContainer` architectures, single-channel input
and condition, LeakyReLU(0.01) layers, an inline layer-array head. Refused with a named error: FiLM
modulation, gated activations, grouped convolutions, an active head 1x1, secondary activations,
bottleneck channels differing from layer channels, and any weight vector that disagrees with the
declared geometry.

Conversion happens in two places — `nts-nam-import` and the plug-in itself — and the two are
required to be byte-for-byte identical. `nts_nam_parity` packs every corpus capture at both tiers
with both converters and compares the resulting `model.bin`; a single differing byte fails the
build. That is what allows the plug-in to carry its own reader without the duplicate drifting.

The two differ in one respect, and the artifact records it. `nts-nam-import` writes test vectors
rendered by the Python implementation, which is checked against upstream `neural-amp-modeler`, so
validating them at load time is a parity gate. The plug-in cannot render independent vectors — the
renderer would be the implementation under test — so its artifacts carry
`"testVectorSource": "runtime"` and their vectors establish load, priming and determinism instead.

See [nam-capture-guide.md](nam-capture-guide.md) for the tools and
[nam-integration-plan.md](nam-integration-plan.md) for the format derivation.

# Format compatibility

Versions evolve independently. Application SemVer changes do not imply a package/model/schema change.

| Format | Current writer | Accepted readers | Compatibility rule |
|---|---:|---:|---|
| Application | 0.10.0 | n/a | SemVer; project declares minimum runtime |
| `.tforge` project schema | 6 | 0–6 | Each older version migrates forward in turn; newer versions rejected |
| `.ntone` package | 1 | 1 | Unknown package versions rejected before asset reads |
| Packed neural model | 2 | 1–2 | Architecture/version pair, dimensions, size and test vector must validate |
| Circuit graph | 1 | 0–1 | v0 migrates to v1; newer versions rejected |
| Dataset session | 1 | 1 | Exact schema plus provenance/rights validation |
| Tone analysis / embedding | 1 | 1 | Version participates in comparison and persistence |
| Source separation | 1 | 1 | Backend identity/version recorded with cache results |

A `.ntone` package may now carry the cabinet responses a rig uses, as `cabinet-ir` assets with a
`cabinetSlots` array in the manifest saying which slot each belongs to. The mapping is separate from the asset
paths because the exporter names those by index, so a package carrying only slot B's response would otherwise
be indistinguishable from one carrying only slot A's. An absent `cabinetSlots` means the package carries no
cabinet responses, which is what every package written before this has and remains the default: embedding is
opt-in, because most impulse responses are licensed for use rather than redistribution.

Schema 6 also records a SHA-256 of each loaded cabinet response's bytes beside its path. A path alone cannot
tell "the file moved" apart from "the file at that path is not the one you saved", and the second silently
re-voices a mix. An empty digest — which is what every earlier project has — means the question cannot be
answered, which the plug-in treats differently from answering it wrongly.

Schema 6 adds the cabinet stage's own controls in a `cabinet` block of their own, rather than
extending `engine.ampControls`. That list is nearly at the 64-entry ceiling `validate` enforces, and
overrunning it does not truncate a save — it makes every saved project fail to load. A project
written before schema 6 carries no cabinet block, and that is a complete description rather than a
gap: the defaults for those controls are the values the fields held while they were unreachable, so
an older project restores the cabinet it actually described.

`.ntone` manifest `minimumRuntime` is checked independently of `packageFormat`. A profile may therefore use
format 1 but require a later 0.x runtime because of a new rig control or model architecture. The browser shows
such a profile as incompatible and will not apply it. Older supported model files remain readable; writers use
the current version. There is no silent fallback for unknown package, graph, dataset, analysis, or separation
schemas.
