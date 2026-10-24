from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

from packetwhisperer.llm import DEFAULT_EFFORT, DEFAULT_MODEL


@dataclass(frozen=True)
class Settings:
    """Service configuration, read from environment variables (see deploy/.env.example)."""

    api_keys: tuple[str, ...] = ()  # empty = auth disabled (local dev only)
    model: str = DEFAULT_MODEL
    effort: str = DEFAULT_EFFORT
    engine: str = "auto"
    max_upload_bytes: int = 50 * 1024 * 1024
    max_flows: int = 20
    usage_log: Path = field(default_factory=lambda: Path("usage.jsonl"))
    slack_webhook_url: str | None = None

    @classmethod
    def from_env(cls) -> Settings:
        env = os.environ
        keys = tuple(k.strip() for k in env.get("PW_API_KEYS", "").split(",") if k.strip())
        return cls(
            api_keys=keys,
            model=env.get("PW_MODEL", DEFAULT_MODEL),
            effort=env.get("PW_EFFORT", DEFAULT_EFFORT),
            engine=env.get("PW_ENGINE", "auto"),
            max_upload_bytes=int(env.get("PW_MAX_UPLOAD_MB", "50")) * 1024 * 1024,
            max_flows=int(env.get("PW_MAX_FLOWS", "20")),
            usage_log=Path(env.get("PW_USAGE_LOG", "usage.jsonl")),
            slack_webhook_url=env.get("SLACK_WEBHOOK_URL") or None,
        )
