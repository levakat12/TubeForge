# TubeForge `.ntone` package format 1

TubeForge 0.10 uses a directory container whose name ends in `.ntone`. This intentionally simple first
community format can be copied through normal file-sharing tools without requiring a server. Import never
executes package content.

## Layout

```text
example.ntone/
  manifest.json
  rig.json
  model.bin              optional packed NTSM model
  cabinet/*.wav          optional licensed IRs
  preview/*.{wav,flac}   optional previews
  license.txt            optional asset terms
```

`manifest.json` contains `packageFormat: 1`, a UUID, name, author, instrument, creator and minimum-runtime
versions, creation time, tags, quality, supported sample rates, model operator allowlist, package license,
`sourceAudioIncluded: false`, and an asset table. Every asset entry declares its relative path, role, exact
byte count, SHA-256, license class, and redistribution permission. Optional signatures use
`rsa-sha256-raw-v1` and a trusted signer ID over canonical manifest JSON without the signature fields.

## Import security

The importer permits only the documented paths and media extensions. It rejects absolute/backslash paths,
`.` or `..`, symlinks, undeclared or duplicate files, executables, size/count overages, hash mismatches,
invalid amp presets, unsupported model operators, malformed packed models, untrusted signatures, and source
audio. Limits are 1 MiB manifest, 2 MiB rig, 64 MiB model, 16 MiB per IR, 30 MiB per preview, 128 files,
and 128 MiB total. Model dimensions, finite weights, architecture, schema, payload size, and memory are
validated again by the C++ inference loader. Package validation is synchronous metadata/file work and must
not run on the audio thread.

Unsigned packages are permitted for local workflows and carry a visible warning. A distribution channel can
set `allowUnsigned = false` and provide its trusted public-key list.

## Licensing

Asset licenses are `user-owned`, `public-domain`, `permissive`, `private-use`, or `unknown`. Cabinet IR export
requires explicit redistribution permission and a user-owned/public-domain/permissive classification.
Private-use and unknown IRs cannot be repackaged. Package metadata is not a substitute for the underlying
license.
