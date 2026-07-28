# Third-party notices

TubeForge uses JUCE 8.0.13. JUCE is available under the AGPLv3 and commercial licensing options; anyone
distributing TubeForge must satisfy the applicable JUCE terms. Python ML dependencies and their exact versions
are declared in `ml/pyproject.toml` and must be reviewed when producing a redistributable model pack.

The optional local source-separation runtime installs Demucs 4.1 (MIT license) and PyTorch (BSD-style license)
with their transitive Python dependencies into the user's TubeForge application-data directory. These packages
and the `htdemucs_6s` learned weights are downloaded separately and are not embedded in the source repository.
Release producers must preserve the upstream notices and verify the model-weight distribution terms before
bundling the runtime or weights in an installer.

Cabinet impulse responses, datasets, recordings, and learned weights are not automatically covered by the
application source terms. Each distributed asset must carry provenance and redistribution metadata. TubeForge
blocks repackaging an IR marked private-use, unknown, or not redistributable.
