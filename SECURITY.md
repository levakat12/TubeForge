# Security

Report suspected vulnerabilities privately to the repository owner before public disclosure. Include the
affected version, reproduction, impact, and whether a malicious `.ntone`, model, project, or audio file is
required. Do not include copyrighted recordings or private user paths.

TubeForge treats packages and learned models as untrusted data: paths and types are allowlisted; sizes, counts,
schemas, hashes, signatures, licenses, operators, dimensions, finite values, and memory are bounded before use.
Package content is never executed. Release builds must use a valid Authenticode certificate and timestamp;
unsigned CI artifacts are testing-only. Dependencies are pinned and CI builds Debug and Release configurations.
