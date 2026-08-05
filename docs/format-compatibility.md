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
| `.tforge` project schema | 4 | 0–4 | Each older version migrates forward in turn; newer versions rejected |
| `.ntone` package | 1 | 1 | Unknown package versions rejected before asset reads |
| Packed neural model | 2 | 1–2 | Architecture/version pair, dimensions, size and test vector must validate |
| Circuit graph | 1 | 0–1 | v0 migrates to v1; newer versions rejected |
| Dataset session | 1 | 1 | Exact schema plus provenance/rights validation |
| Tone analysis / embedding | 1 | 1 | Version participates in comparison and persistence |
| Source separation | 1 | 1 | Backend identity/version recorded with cache results |

`.ntone` manifest `minimumRuntime` is checked independently of `packageFormat`. A profile may therefore use
format 1 but require a later 0.x runtime because of a new rig control or model architecture. The browser shows
such a profile as incompatible and will not apply it. Older supported model files remain readable; writers use
the current version. There is no silent fallback for unknown package, graph, dataset, analysis, or separation
schemas.
