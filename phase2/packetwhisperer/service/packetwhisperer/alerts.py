"""Slack (or Discord, via its Slack-compatible endpoint) alerts for problem diagnoses."""

from __future__ import annotations

import logging

import httpx

from packetwhisperer.models import Diagnosis

log = logging.getLogger(__name__)


def format_alert(diagnosis: Diagnosis, filename: str) -> str:
    lines = [
        f":rotating_light: *{diagnosis.label}* ({diagnosis.confidence} confidence) in `{filename}`",
        diagnosis.summary,
    ]
    lines += [f"• {e}" for e in diagnosis.evidence[:3]]
    if diagnosis.next_steps:
        lines.append(f"*Next step:* {diagnosis.next_steps[0]}")
    return "\n".join(lines)


def notify_slack(webhook_url: str, diagnosis: Diagnosis, filename: str) -> bool:
    """Best effort: an alerting outage must never fail the diagnosis request."""
    try:
        r = httpx.post(webhook_url, json={"text": format_alert(diagnosis, filename)}, timeout=5.0)
        r.raise_for_status()
        return True
    except httpx.HTTPError as e:
        log.warning("Slack alert failed: %s", e)
        return False
