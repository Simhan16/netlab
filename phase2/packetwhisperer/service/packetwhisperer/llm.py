"""LLM analysis layer: flow summary -> root cause, via Claude.

The model gets the compacted JSON summary (never raw bytes) and must answer
in a JSON schema whose `label` is an enum of the taxonomy, so every response
is machine-checkable against the casebook labels.
"""

from __future__ import annotations

import json
from typing import Any

import anthropic

from packetwhisperer.models import Diagnosis, Usage
from packetwhisperer.taxonomy import LABELS

DEFAULT_MODEL = "claude-opus-5-5"
DEFAULT_EFFORT = "medium"

# "default" lets the API pick a fallback model, routed by refusal category,
# if the primary model declines. Packet captures of attacks or scans are
# legitimate here, but can look like security content.
FALLBACK_BETA = "server-side-fallback-2026-07-01"


class LLMUnavailable(Exception):
    """The API couldn't be reached or rejected the request; callers may fall back to rules."""


class LLMBadResponse(Exception):
    """The model answered, but not with a usable diagnosis."""


DIAGNOSIS_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "label": {"type": "string", "enum": list(LABELS)},
        "confidence": {"type": "string", "enum": ["low", "medium", "high"]},
        "summary": {"type": "string"},
        "evidence": {"type": "array", "items": {"type": "string"}},
        "next_steps": {"type": "array", "items": {"type": "string"}},
    },
    "required": ["label", "confidence", "summary", "evidence", "next_steps"],
    "additionalProperties": False,
}

_LABEL_LINES = "\n".join(f"- {name}: {desc}" for name, desc in LABELS.items())

SYSTEM_PROMPT = f"""You are a senior network engineer helping campus IT staff figure out why something on \
the network is broken. You are given a summary of one packet capture, computed by a packet analyzer, and \
you identify the single most likely root cause.

How to read the summary:
- It describes traffic as seen from one capture point. RTTs are measured from that point, and a fault \
looks different depending on whether the capture is near the client, the server, or a router in between.
- In TCP flows, two-element arrays are [client->server, server->client]. `state` is how the connection \
went: syn_unanswered (SYNs got no reply), refused (SYN answered by RST), synack_unacked (server replied \
but the client never completed the handshake), open/closed/half_closed/reset (after a completed \
handshake), midstream* (the capture began after the connection opened).
- `retransmissions` are segments re-sent with data already seen; `bytes_acked` is how much of each side's \
data the other side acknowledged, i.e. real progress.
- `rtt_ms.client_data` is the time from seeing client data to seeing the server's ACK for it, and \
`server_data` the reverse. Near the client, client_data is the network round trip and server_data is just \
the client's own ACK delay. `handshake_ms` splits the handshake the same way.
- `dns.unanswered` are queries with no response in the capture; `dns.failures` are non-NOERROR answers.
- `icmp.errors` quote the packet that caused them in `about`.
- Flows are pre-sorted most-suspicious first and may be truncated; `flows_not_shown` says how many.

Root-cause labels:
{_LABEL_LINES}

Answer with:
- label: the primary root cause. Use "healthy" when nothing is wrong, even if there is minor noise, and \
"unknown" when the evidence doesn't support any label.
- confidence: how strongly the capture supports the label.
- summary: two or three plain-English sentences for someone who isn't a network engineer. No jargon \
without a short explanation.
- evidence: the specific facts from the summary that support the label, with the numbers.
- next_steps: concrete things IT staff should check or change."""


def _user_message(summary: dict[str, Any], context: str | None) -> str:
    # sort_keys keeps the serialized summary byte-stable for identical captures.
    body = json.dumps(summary, sort_keys=True, separators=(",", ":"))
    note = f"Reported problem: {context}\n\n" if context else ""
    return f"{note}Capture summary:\n{body}"


def diagnose_llm(
    summary: dict[str, Any],
    *,
    context: str | None = None,
    client: anthropic.Anthropic | None = None,
    model: str = DEFAULT_MODEL,
    effort: str = DEFAULT_EFFORT,
) -> Diagnosis:
    """Diagnose a (compacted) summary. Raises LLMUnavailable or LLMBadResponse."""
    if client is None:
        client = anthropic.Anthropic()  # env key, auth token, or an `ant auth login` profile
        # With nothing configured the SDK only fails at request time, with a bare TypeError.
        if client.api_key is None and client.auth_token is None and client.credentials is None:
            raise LLMUnavailable("no Claude API credentials configured (set ANTHROPIC_API_KEY)")
    try:
        response = client.beta.messages.create(
            model=model,
            max_tokens=16000,
            betas=[FALLBACK_BETA],
            fallbacks="default",
            output_config={
                "effort": effort,
                "format": {"type": "json_schema", "schema": DIAGNOSIS_SCHEMA},
            },
            system=SYSTEM_PROMPT,
            messages=[{"role": "user", "content": _user_message(summary, context)}],
        )
    except (anthropic.AuthenticationError, anthropic.PermissionDeniedError) as e:
        raise LLMUnavailable(f"Claude API credentials rejected: {e.message}") from e
    except anthropic.RateLimitError as e:
        raise LLMUnavailable("Claude API rate limit reached") from e
    except anthropic.BadRequestError as e:
        raise LLMBadResponse(f"request rejected: {e.message}") from e
    except anthropic.APIStatusError as e:
        raise LLMUnavailable(f"Claude API error {e.status_code}") from e
    except anthropic.APIConnectionError as e:
        raise LLMUnavailable("could not reach the Claude API") from e

    # Check why generation stopped before trusting the content.
    if response.stop_reason == "refusal":
        category = getattr(response.stop_details, "category", None) if response.stop_details else None
        raise LLMBadResponse(f"model declined to analyze this capture (category: {category})")
    if response.stop_reason == "max_tokens":
        raise LLMBadResponse("response was cut off at max_tokens")

    text = next((b.text for b in response.content if b.type == "text"), None)
    if text is None:
        raise LLMBadResponse("response contained no text block")
    try:
        fields = json.loads(text)
        diagnosis = Diagnosis(**fields, engine="llm")
    except (json.JSONDecodeError, TypeError, ValueError) as e:
        raise LLMBadResponse(f"response didn't match the diagnosis schema: {e}") from e

    diagnosis.usage = Usage(
        input_tokens=response.usage.input_tokens,
        output_tokens=response.usage.output_tokens,
        model=response.model,  # may differ from `model` if a fallback served the request
    )
    return diagnosis
