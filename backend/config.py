from __future__ import annotations

import os
from pathlib import Path
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parent.parent


def load_config(root: Path = ROOT) -> dict[str, Any]:
    config = yaml.safe_load((root / "config.yaml").read_text(encoding="utf-8"))
    # Minimal .env support without another dependency; process env takes precedence.
    env = dict(os.environ)
    dotenv = root / ".env"
    if dotenv.exists():
        for line in dotenv.read_text(encoding="utf-8-sig").splitlines():
            if "=" in line and not line.lstrip().startswith("#"):
                name, value = line.split("=", 1)
                env.setdefault(name.strip(), value.strip().strip("\"'"))
    llm = config["llm"]
    llm["api_key"] = env.get("KHYPER_API_KEY") or llm.get("api_key", "")
    llm["base_url"] = env.get("KHYPER_URL") or env.get("KHYPER_BASE_URL") or llm.get("url", "")
    llm["model"] = env.get("KHYPER_MODEL_NAME") or env.get("KHYPER_MODEL") or llm.get("model_name", "")
    if env.get("KHYPER_PORT"):
        config["server"]["port"] = int(env["KHYPER_PORT"])
    for name in ("max_tool_calls", "max_model_calls"):
        if not 1 <= config["agent"][name] <= 20:
            raise ValueError(f"agent.{name} must be between 1 and 20")
    return config


def project_path(value: str, root: Path = ROOT) -> Path:
    p = Path(value)
    return p if p.is_absolute() else root / p
