from __future__ import annotations

import html
import json
from pathlib import Path
from typing import Any


def write_alignment_html(validation_report: Path, destination: Path) -> None:
    report: dict[str, Any] = json.loads(validation_report.read_text(encoding="utf-8"))
    takes = report.get("takes", [])
    rows: list[str] = []
    charts: list[str] = []
    for take in takes:
        alignment = take.get("alignment", {})
        latencies = [int(value) for value in alignment.get("localLatencies", [])]
        rows.append(
            "<tr>"
            f"<td>{html.escape(str(take.get('takeId', '')))}</td>"
            f"<td>{alignment.get('globalLatencySamples', 0)}</td>"
            f"<td>{alignment.get('driftPpm', 0):.3f}</td>"
            f"<td>{alignment.get('confidence', 0):.4f}</td>"
            f"<td>{take.get('missingOutputRatio', 0):.5f}</td>"
            "</tr>")
        if latencies:
            low, high = min(latencies), max(latencies)
            span = max(1, high - low)
            points = " ".join(
                f"{20 + index * 560 / max(1, len(latencies) - 1):.1f},{100 - (value - low) * 80 / span:.1f}"
                for index, value in enumerate(latencies))
            charts.append(f"<h3>{html.escape(str(take.get('takeId', '')))} local latency</h3>"
                          f"<svg viewBox='0 0 600 120'><polyline points='{points}'/></svg>")
    document = f"""<!doctype html>
<html><head><meta charset="utf-8"><title>TubeForge alignment report</title>
<style>body{{font:14px system-ui;background:#15171c;color:#eee;margin:32px}} table{{border-collapse:collapse}}
td,th{{padding:8px 12px;border:1px solid #555}} polyline{{fill:none;stroke:#efa64d;stroke-width:2}}
svg{{max-width:900px;background:#222;border:1px solid #555}}</style></head>
<body><h1>Alignment report</h1><p>Session: {html.escape(str(report.get('sessionId', '')))}</p>
<p>Accepted: <strong>{report.get('accepted', False)}</strong></p>
<table><thead><tr><th>Take</th><th>Latency</th><th>Drift ppm</th><th>Confidence</th><th>Missing output</th></tr></thead>
<tbody>{''.join(rows)}</tbody></table>{''.join(charts)}</body></html>"""
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(document, encoding="utf-8")
