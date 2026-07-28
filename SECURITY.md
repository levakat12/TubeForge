# Security

Report suspected vulnerabilities privately to the repository owner before public disclosure. Include the
affected version, reproduction, impact, and whether a malicious `.ntone`, model, project, or audio file is
required. Do not include copyrighted recordings or private user paths.

TubeForge treats packages and learned models as untrusted data: paths and types are allowlisted; sizes, counts,
schemas, hashes, signatures, licenses, operators, dimensions, finite values, and memory are bounded before use.
Package content is never executed. Release builds must use a valid Authenticode certificate and timestamp;
unsigned CI artifacts are testing-only. Dependencies are pinned and CI builds Debug and Release configurations.

Package and update manifests are signed with RSASSA-PKCS1-v1_5 over SHA-256 (RFC 8017), identified as
`rsa-pkcs1-sha256-v1` and requiring a minimum 2048-bit modulus. The algorithm identifier is covered by the
signature, so a manifest cannot be downgraded in transit. Verification rebuilds the expected encoded message
and compares it whole rather than parsing the recovered value.

The earlier `rsa-sha256-raw-v1` scheme applied the RSA primitive to an unpadded SHA-256 digest and is
rejected. It was forgeable: raw RSA is multiplicative, and because JUCE key generation prefers a public
exponent of 3, a 256-bit digest inside a larger modulus could be recovered by integer cube root without the
private key. Any `.ntone` package or update manifest signed with the old identifier must be re-signed.
