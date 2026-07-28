from __future__ import annotations

from pathlib import Path
import argparse
import json

from nts_ml.capture import CaptureConfig, CaptureWizard


parser = argparse.ArgumentParser(description="TubeForge neural amplifier capture wizard")
subcommands = parser.add_subparsers(dest="command", required=True)
initialize = subcommands.add_parser("init"); initialize.add_argument("root", type=Path)
initialize.add_argument("--sample-rate", type=int, default=48_000)
initialize.add_argument("--target-type", default="amplifier"); initialize.add_argument("--target-name", default="User capture")
initialize.add_argument("--input-channel", type=int, default=0); initialize.add_argument("--return-channel", type=int, default=0)
initialize.add_argument("--expected-rms-db", type=float, default=-21.0)
initialize.add_argument("--duration", type=float, default=120.0); initialize.add_argument("--acknowledge-feedback-risk", action="store_true")
inspect = subcommands.add_parser("inspect"); inspect.add_argument("root", type=Path)
train_parser = subcommands.add_parser("train"); train_parser.add_argument("root", type=Path)
train_parser.add_argument("--config", type=Path, required=True); train_parser.add_argument("--train-session", type=Path, action="append", required=True)
train_parser.add_argument("--validation-session", type=Path, action="append", required=True)
activate = subcommands.add_parser("activate"); activate.add_argument("root", type=Path); activate.add_argument("artifact", type=Path)
options = parser.parse_args(); wizard = CaptureWizard(options.root)
if options.command == "init":
    result = wizard.initialize(CaptureConfig(options.input_channel, options.return_channel, options.sample_rate,
        options.target_type, options.target_name, options.expected_rms_db, options.duration, None,
        options.acknowledge_feedback_risk))
elif options.command == "inspect": result = wizard.inspect_capture()
elif options.command == "train":
    artifact, quality = wizard.train_capture(options.train_session, options.validation_session, options.config)
    result = {"artifact": str(artifact), "quality": quality}
else: result = {"activeModel": str(wizard.activate(options.artifact))}
print(json.dumps(result, indent=2, sort_keys=True))
