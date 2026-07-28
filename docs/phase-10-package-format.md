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
`rsa-pkcs1-sha256-v1` and a trusted signer ID over canonical manifest JSON without the signature fields.

Signatures are RSASSA-PKCS1-v1_5 over SHA-256 (RFC 8017) with a minimum 2048-bit modulus, encoded as a
fixed-width lowercase hex value exactly as wide as the modulus. Verification rebuilds the whole expected
encoded message and compares it in full rather than parsing the recovered value. The signature covers the
algorithm identifier, so a manifest cannot be downgraded to a weaker scheme in transit.

This replaces `rsa-sha256-raw-v1`, which applied the RSA primitive to a bare SHA-256 digest with no padding.
That construction was forgeable: JUCE key generation prefers a public exponent of 3, and an unpadded 256-bit
digest inside a 2048-bit modulus can be recovered by integer cube root without the private key. Packages and
update manifests carrying the old identifier are rejected.

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
