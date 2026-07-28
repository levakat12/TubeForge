# Autonomy & Direct Action Rules

- **Bias Toward Action:** Never stall by asking multiple clarifying questions. If a request is high-level, make reasonable domain assumptions and start implementing immediately.
- **Inspect Code First:** Always check the repository map and existing DSP/ML files before asking where functionality belongs.
- **Draft Code Immediately:** Provide a working implementation or initial code stubs first. State your technical assumptions briefly at the *end* of your edits rather than blocking execution upfront.
- **Guitar DSP/ML Assumptions:** For tasks involving "high gain tone analysis", assume standard feature extraction (STFT, Mel-Spectrograms, spectral centroid, harmonic distortion, gain clipping detection, or neural amp modeling/NAM) unless explicitly told otherwise.

# Autonomous Web Research & Documentation

- **Proactive Web Fetching:** If you need C++ library specifications, API references, DSP formulas, or ML architecture details (e.g., JUCE, FFTW, Eigen, Librosa C++ equivalents, Neural Amp Modeler specifications), do not ask the user to paste them. Proactively request to scrape the relevant documentation URL or use Aider's `/web <url>` capability.
- **Self-Serve Research:** When encountering unfamiliar third-party APIs, C++20 features, or audio DSP algorithms, identify the official documentation or GitHub repository URL, request to fetch the page, and proceed with the implementation autonomously.
- **Extract & Implement:** After retrieving web documentation, extract the required function signatures or mathematical formulations immediately and integrate them into code diffs without unnecessary back-and-forth.