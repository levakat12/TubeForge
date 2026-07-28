# Research and reproducibility

Evaluation must record the Git commit, application/model/dataset format versions, sample rate, block size,
random seed, corpus split IDs, hardware, compiler, and command line. Dataset splits are performance-aware;
takes from one performance cannot cross train/validation/test boundaries. Capture provenance and performer
release checks are mandatory. Report spectral, temporal, loudness, nonlinear, null/error, CPU, memory, and
latency metrics together rather than selecting a single favorable score.

The repository includes deterministic DI fixtures, C++/Python packed-model parity, benchmark acceptance
targets, corpus audit tools, experiment tracking, ABX utilities, and one-hour scheduled soak testing. Generated
model and dataset artifacts must publish their SHA-256 and applicable license without publishing restricted
source audio. Claims about subjective similarity require the documented level-matched blinded protocol.
