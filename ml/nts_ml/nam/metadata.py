from __future__ import annotations

from dataclasses import dataclass
from typing import Any


@dataclass(frozen=True, slots=True)
class CaptureMetadata:
    """The provenance fields a NAM file carries about the hardware it was captured from.

    Everything here is optional in the format, so every field has a defined empty value rather
    than raising. `loudness` is the one that matters to the runtime: it is the measured LUFS of
    the capture's output, and it is what `normalization.json` should carry rather than the
    runtime's -21 dBFS default.
    """

    name: str = ""
    modeled_by: str = ""
    gear_make: str = ""
    gear_model: str = ""
    gear_type: str = ""
    tone_type: str = ""
    loudness: float | None = None
    gain: float | None = None
    validation_esr: float | None = None

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> CaptureMetadata:
        data = data or {}
        training = data.get("training") or {}
        esr = training.get("validation_esr") if isinstance(training, dict) else None
        def text(key: str) -> str: return str(data.get(key) or "")
        def number(value: Any) -> float | None:
            return float(value) if isinstance(value, int | float) and not isinstance(value, bool) else None
        return cls(text("name"), text("modeled_by"), text("gear_make"), text("gear_model"),
                   text("gear_type"), text("tone_type"), number(data.get("loudness")),
                   number(data.get("gain")), number(esr))

    def to_dict(self) -> dict[str, Any]:
        return {"name": self.name, "modeledBy": self.modeled_by, "gearMake": self.gear_make,
                "gearModel": self.gear_model, "gearType": self.gear_type, "toneType": self.tone_type,
                "loudness": self.loudness, "gain": self.gain, "validationEsr": self.validation_esr}

    def license_text(self) -> str:
        """The attribution line an exported artifact carries.

        Conversion does not grant redistribution rights; the artifact says who made the capture
        so that the question is answerable later instead of lost.
        """
        author = self.modeled_by or "an unnamed author"
        subject = self.name or "an unnamed capture"
        return (f"Derived from the Neural Amp Modeler capture '{subject}' by {author}.\n"
                "Converted for local use. Redistribution requires the capture author's permission.\n")
