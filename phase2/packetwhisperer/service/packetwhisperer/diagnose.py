"""Entry point tying extraction and the two diagnosers together."""

from __future__ import annotations

import logging
from typing import Any, Literal

import anthropic

from packetwhisperer.extract import compact
from packetwhisperer.llm import DEFAULT_EFFORT, DEFAULT_MODEL, LLMBadResponse, LLMUnavailable, diagnose_llm
from packetwhisperer.models import Diagnosis
from packetwhisperer.rules import diagnose_rules

log = logging.getLogger(__name__)

Engine = Literal["rules", "llm", "auto"]


def diagnose(
    summary: dict[str, Any],
    engine: Engine = "auto",
    *,
    context: str | None = None,
    max_flows: int = 20,
    client: anthropic.Anthropic | None = None,
    model: str = DEFAULT_MODEL,
    effort: str = DEFAULT_EFFORT,
) -> Diagnosis:
    """Diagnose a full analyzer summary.

    engine="llm" raises if Claude is unavailable; "auto" degrades to the
    rules engine instead and says so in `note`, so a user still gets an answer.
    """
    if engine == "rules":
        return diagnose_rules(summary)
    try:
        return diagnose_llm(compact(summary, max_flows), context=context, client=client, model=model, effort=effort)
    except (LLMUnavailable, LLMBadResponse) as e:
        if engine == "llm":
            raise
        log.warning("LLM diagnosis failed, falling back to rules: %s", e)
        result = diagnose_rules(summary)
        result.note = f"AI analysis unavailable ({e}); showing rule-based diagnosis"
        return result
