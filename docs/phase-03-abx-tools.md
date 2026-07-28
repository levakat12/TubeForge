# Phase 3 ABX evidence tools

The ABX tools turn real rendered performances into a reproducible evidence pack. They do not create
or substitute for human listening results.

## Prepare sources

Copy `ml/templates/abx-sources.json` beside the rendered candidates and add one entry for every source.
The accepted category names are the eight names in `phase-03-listening-protocol.md`. Candidate files
must have the same sample rate, channel count, frame count, and PCM WAV format.

From the repository root, set `PYTHONPATH` to `ml`, then create the pack:

```powershell
$env:PYTHONPATH = "ml"
python ml/scripts/create_abx_pack.py D:\listening\abx-sources.json D:\listening\pack --trials 12
```

The creator discards the first 500 ms, measures gated integrated BS.1770 loudness, trims both files
down to the quieter target, verifies a maximum 0.2 LU difference, randomizes A/B and X independently,
and stores SHA-256 hashes. Keep `answer-key.json` hidden from listeners. Give each listener a separate
copy of `responses.csv`; every completed row needs a participant ID and A/B selection. The included
`player.html` loads all three buffers synchronously and uses a 10 ms ramp for click-free A/B/X changes.
Serve the pack with `python -m http.server --directory D:\listening\pack`, open the displayed local URL,
and export the completed CSV from the player. The seven
preference fields accept scores from 1 to 7.

## Analyze responses

```powershell
python ml/scripts/analyze_abx.py D:\listening\pack `
  D:\listening\listener-01.csv D:\listening\listener-01-report.json
```

The report contains exact one-sided binomial probabilities, 95% Wilson intervals, preference means,
and per-category and aggregate results. A category is marked identifiable only with at least 12 trials
and `p < 0.05`. A release evidence set is complete only after all eight categories contain genuine
listener responses.
