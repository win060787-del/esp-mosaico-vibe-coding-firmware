#!/usr/bin/env python3
"""Local-only, persistent ASR worker. stdout is a JSON-lines protocol."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import sys

os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", HF_HUB_DISABLE_TELEMETRY="1")

def reply(value):
    print(json.dumps(value, ensure_ascii=False), flush=True)

def main():
    config = json.loads(Path(sys.argv[1]).read_text())
    model_dir = Path(config["modelPath"]).expanduser()
    if not model_dir.is_absolute():
        model_dir = Path(sys.argv[1]).resolve().parent / model_dir
    with (model_dir / "model.safetensors").open("rb") as source:
        if hashlib.file_digest(source, "sha256").hexdigest() != config["modelSHA256"]:
            raise ValueError("Qwen model SHA-256 verification failed")
    with contextlib.redirect_stdout(sys.stderr):
        from mlx_audio.stt import load
        model = load(str(model_dir))
    reply({"ready": True})
    for line in sys.stdin:
        request = json.loads(line)
        try:
            with contextlib.redirect_stdout(sys.stderr):
                result = model.generate(request["audio"], language="Chinese", max_tokens=2048, min_chunk_duration=0.25)
            reply({"id": request["id"], "text": result.text.strip()})
        except Exception as error:
            reply({"id": request["id"], "error": str(error)})

if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        reply({"error": str(error)})
        sys.exit(1)
