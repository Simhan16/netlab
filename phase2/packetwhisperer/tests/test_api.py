import json
from pathlib import Path

import pytest
from fastapi.testclient import TestClient
from packetwhisperer.api import create_app
from packetwhisperer.settings import Settings
from packetwhisperer.usage import read_entries, summarize


@pytest.fixture
def settings(tmp_path: Path) -> Settings:
    return Settings(api_keys=("s3cret",), engine="rules", usage_log=tmp_path / "usage.jsonl",
                    max_upload_bytes=1024 * 1024)


def upload(client: TestClient, pcap: Path, key: str | None = "s3cret", **form: str):
    headers = {"X-API-Key": key} if key else {}
    with pcap.open("rb") as f:
        return client.post("/v1/diagnose", files={"file": (pcap.name, f)}, data=form, headers=headers)


def test_diagnose_and_usage_log(settings: Settings, scenario_pcaps: dict[str, Path]) -> None:
    client = TestClient(create_app(settings))
    r = upload(client, scenario_pcaps["dns_nxdomain"], context="can't reach the API")
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["diagnosis"]["label"] == "dns_nxdomain"
    assert body["capture"]["packets"] == 4

    (entry,) = read_entries(settings.usage_log)
    assert entry["status"] == "ok" and entry["label"] == "dns_nxdomain"
    assert entry["caller"] != "s3cret" and len(entry["caller"]) == 8  # hashed id, never the key
    assert summarize([entry])["diagnosed"] == 1

    r = client.post("/v1/feedback", json={"request_id": body["request_id"], "correct": True},
                    headers={"X-API-Key": "s3cret"})
    assert r.status_code == 200
    assert read_entries(settings.usage_log)[-1]["status"] == "feedback"


def test_auth_required(settings: Settings, scenario_pcaps: dict[str, Path]) -> None:
    client = TestClient(create_app(settings))
    assert upload(client, scenario_pcaps["healthy"], key=None).status_code == 401
    assert upload(client, scenario_pcaps["healthy"], key="wrong").status_code == 401
    assert client.get("/healthz").status_code == 200


def test_rejects_bad_input(settings: Settings, tmp_path: Path) -> None:
    client = TestClient(create_app(settings))
    junk = tmp_path / "junk.pcap"
    junk.write_bytes(b"hello")
    assert upload(client, junk).status_code == 400

    big = tmp_path / "big.pcap"
    big.write_bytes(b"\0" * (2 * 1024 * 1024))
    assert upload(client, big).status_code == 413

    pcap = tmp_path / "x.pcap"
    pcap.write_bytes(b"\0")
    assert upload(client, pcap, engine="magic").status_code == 422


def test_slack_alert_sent_for_problems(settings: Settings, scenario_pcaps: dict[str, Path],
                                       monkeypatch: pytest.MonkeyPatch) -> None:
    sent: list[str] = []
    monkeypatch.setattr("packetwhisperer.api.notify_slack", lambda url, d, name: sent.append(d.label))
    client = TestClient(create_app(Settings(**{**settings.__dict__, "slack_webhook_url": "https://hooks.example"})))
    upload(client, scenario_pcaps["firewall_drop"])
    upload(client, scenario_pcaps["healthy"])
    assert sent == ["firewall_drop"]


def test_index_page(settings: Settings) -> None:
    r = TestClient(create_app(settings)).get("/")
    assert r.status_code == 200 and "PacketWhisperer" in r.text
    json.dumps(r.text)  # plain string
