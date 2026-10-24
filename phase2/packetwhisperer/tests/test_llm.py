import json
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import anthropic
import httpx
import pytest
from packetwhisperer.diagnose import diagnose
from packetwhisperer.extract import extract
from packetwhisperer.llm import DIAGNOSIS_SCHEMA, LLMBadResponse, LLMUnavailable, diagnose_llm


class FakeMessages:
    def __init__(self, response: Any = None, error: Exception | None = None) -> None:
        self.response, self.error, self.calls = response, error, []

    def create(self, **kwargs: Any) -> Any:
        self.calls.append(kwargs)
        if self.error:
            raise self.error
        return self.response


def fake_client(response: Any = None, error: Exception | None = None) -> Any:
    return SimpleNamespace(beta=SimpleNamespace(messages=FakeMessages(response, error)))


def reply(fields: dict[str, Any] | str, stop_reason: str = "end_turn") -> Any:
    text = fields if isinstance(fields, str) else json.dumps(fields)
    return SimpleNamespace(
        stop_reason=stop_reason, stop_details=None, model="claude-opus-5-5",
        content=[SimpleNamespace(type="thinking", thinking=""), SimpleNamespace(type="text", text=text)],
        usage=SimpleNamespace(input_tokens=1234, output_tokens=210),
    )


GOOD = {
    "label": "firewall_drop", "confidence": "high", "summary": "A firewall drops the connection.",
    "evidence": ["4 SYNs, no reply"], "next_steps": ["Check security groups"],
}


def test_parses_structured_response() -> None:
    client = fake_client(reply(GOOD))
    d = diagnose_llm({"tcp": {"flows": []}}, client=client, context="site won't load")
    assert d.label == "firewall_drop" and d.engine == "llm"
    assert d.usage and d.usage.input_tokens == 1234
    call = client.beta.messages.calls[0]
    assert call["model"] == "claude-opus-5-5"
    assert call["fallbacks"] == "default" and call["betas"] == ["server-side-fallback-2026-07-01"]
    assert call["output_config"]["format"]["schema"] is DIAGNOSIS_SCHEMA
    assert "site won't load" in call["messages"][0]["content"]


def test_schema_label_enum_matches_taxonomy() -> None:
    from packetwhisperer.taxonomy import LABELS

    assert DIAGNOSIS_SCHEMA["properties"]["label"]["enum"] == list(LABELS)


@pytest.mark.parametrize("response", [
    reply(GOOD, stop_reason="refusal"),
    reply(GOOD, stop_reason="max_tokens"),
    reply("not json"),
    reply({**GOOD, "label": "aliens"}),
])
def test_bad_responses_raise(response: Any) -> None:
    with pytest.raises(LLMBadResponse):
        diagnose_llm({}, client=fake_client(response))


def _api_error(cls: type[anthropic.APIStatusError], status: int) -> anthropic.APIStatusError:
    request = httpx.Request("POST", "https://api.anthropic.com/v1/messages")
    return cls("boom", response=httpx.Response(status, request=request), body=None)


def test_auto_falls_back_to_rules_when_api_down(scenario_pcaps: dict[str, Path]) -> None:
    summary = extract(scenario_pcaps["connection_refused"]).summary
    client = fake_client(error=_api_error(anthropic.RateLimitError, 429))
    d = diagnose(summary, "auto", client=client)
    assert d.engine == "rules" and d.label == "connection_refused"
    assert d.note and "unavailable" in d.note
    with pytest.raises(LLMUnavailable):
        diagnose(summary, "llm", client=client)


def test_missing_credentials_is_unavailable(monkeypatch: pytest.MonkeyPatch, scenario_pcaps: dict[str, Path]) -> None:
    no_creds = SimpleNamespace(api_key=None, auth_token=None, credentials=None)
    monkeypatch.setattr(anthropic, "Anthropic", lambda: no_creds)
    with pytest.raises(LLMUnavailable, match="credentials"):
        diagnose_llm({})
    d = diagnose(extract(scenario_pcaps["firewall_drop"]).summary, "auto")
    assert d.engine == "rules" and d.label == "firewall_drop"


def test_llm_receives_compacted_summary(scenario_pcaps: dict[str, Path]) -> None:
    client = fake_client(reply(GOOD))
    diagnose(extract(scenario_pcaps["healthy"]).summary, "llm", client=client)
    content = client.beta.messages.calls[0]["messages"][0]["content"]
    assert '"flows_not_shown":0' in content
