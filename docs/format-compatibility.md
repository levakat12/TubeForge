# Format compatibility

Versions evolve independently. Application SemVer changes do not imply a package/model/schema change.

| Format | Current writer | Accepted readers | Compatibility rule |
|---|---:|---:|---|
| Application | 0.10.0 | n/a | SemVer; project declares minimum runtime |
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
