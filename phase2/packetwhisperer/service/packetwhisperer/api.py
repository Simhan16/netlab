"""HTTP API: upload a pcap, get a diagnosis.

    POST /v1/diagnose   multipart `file` (+ optional `context`, `engine`)  -> diagnosis
    POST /v1/feedback   {request_id, correct, actual_label?, comment?}    -> recorded
    GET  /healthz
    GET  /              a minimal upload page for non-engineers

Run: uvicorn packetwhisperer.api:app --host 0.0.0.0 --port 8000
"""

# No `from __future__ import annotations` here: FastAPI resolves Depends() in
# annotations at runtime, and the closure-local `caller` must be visible to it.
import hashlib
import hmac
import logging
import tempfile
import time
import uuid
from pathlib import Path
from typing import Annotated, Any

from fastapi import BackgroundTasks, Depends, FastAPI, File, Form, Header, HTTPException, UploadFile
from fastapi.responses import HTMLResponse
from pydantic import BaseModel

from packetwhisperer import __version__
from packetwhisperer.alerts import notify_slack
from packetwhisperer.diagnose import diagnose
from packetwhisperer.extract import extract
from packetwhisperer.llm import LLMBadResponse, LLMUnavailable
from packetwhisperer.settings import Settings
from packetwhisperer.taxonomy import LABELS
from packetwhisperer.usage import UsageLog

log = logging.getLogger(__name__)

ENGINES = ("auto", "rules", "llm")


class Feedback(BaseModel):
    request_id: str
    correct: bool
    actual_label: str | None = None
    comment: str | None = None


def create_app(settings: Settings | None = None) -> FastAPI:
    settings = settings or Settings.from_env()
    usage = UsageLog(settings.usage_log)
    app = FastAPI(title="PacketWhisperer", version=__version__)

    def caller(x_api_key: Annotated[str | None, Header()] = None) -> str:
        if not settings.api_keys:
            return "anonymous"
        if x_api_key:
            for key in settings.api_keys:
                if hmac.compare_digest(x_api_key, key):  # constant-time: no timing side channel
                    return hashlib.sha256(key.encode()).hexdigest()[:8]  # log an id, never the key
        raise HTTPException(status_code=401, detail="missing or invalid X-API-Key")

    @app.get("/healthz")
    def healthz() -> dict[str, str]:
        return {"status": "ok", "version": __version__}

    @app.get("/", response_class=HTMLResponse)
    def index() -> str:
        return UPLOAD_PAGE

    # A sync endpoint: FastAPI runs it in a worker thread, and the C++ parser
    # releases the GIL, so concurrent uploads are analyzed in parallel.
    @app.post("/v1/diagnose")
    def diagnose_upload(
        background: BackgroundTasks,
        file: Annotated[UploadFile, File()],
        who: Annotated[str, Depends(caller)],
        context: Annotated[str | None, Form(max_length=500)] = None,
        engine: Annotated[str | None, Form()] = None,
    ) -> dict[str, Any]:
        engine = engine or settings.engine
        if engine not in ENGINES:
            raise HTTPException(status_code=422, detail=f"engine must be one of {ENGINES}")
        request_id = uuid.uuid4().hex[:12]
        started = time.perf_counter()
        name = Path(file.filename or "upload.pcap").name

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "capture.pcap"
            size = _save_limited(file, path, settings.max_upload_bytes)
            try:
                extraction = extract(path)
            except RuntimeError as e:
                usage.record(request_id=request_id, caller=who, status="bad_pcap", bytes=size)
                raise HTTPException(status_code=400, detail=f"not a readable pcap: {e}") from e

        try:
            result = diagnose(
                extraction.summary, engine, context=context, max_flows=settings.max_flows,
                model=settings.model, effort=settings.effort,  # type: ignore[arg-type]
            )
        except (LLMUnavailable, LLMBadResponse) as e:
            usage.record(request_id=request_id, caller=who, status="llm_error", error=str(e))
            raise HTTPException(status_code=502, detail=str(e)) from e

        latency_ms = round((time.perf_counter() - started) * 1000, 1)
        usage.record(
            request_id=request_id, caller=who, status="ok", filename=name, bytes=size,
            packets=extraction.packets, label=result.label, confidence=result.confidence,
            engine=result.engine, latency_ms=latency_ms,
            input_tokens=result.usage.input_tokens if result.usage else 0,
            output_tokens=result.usage.output_tokens if result.usage else 0,
        )
        if settings.slack_webhook_url and result.label not in ("healthy", "unknown"):
            background.add_task(notify_slack, settings.slack_webhook_url, result, name)

        return {
            "request_id": request_id,
            "diagnosis": result.model_dump(),
            "capture": extraction.summary["capture"],
            "summary_text": extraction.text,
            "latency_ms": latency_ms,
        }

    @app.post("/v1/feedback")
    def feedback(body: Feedback, who: Annotated[str, Depends(caller)]) -> dict[str, str]:
        if body.actual_label is not None and body.actual_label not in LABELS:
            raise HTTPException(status_code=422, detail=f"actual_label must be one of {sorted(LABELS)}")
        usage.record(status="feedback", caller=who, **body.model_dump())
        return {"status": "recorded"}

    return app


def _save_limited(upload: UploadFile, dest: Path, limit: int) -> int:
    """Stream the upload to disk, refusing anything over `limit` bytes without buffering it in memory."""
    size = 0
    with dest.open("wb") as out:
        while chunk := upload.file.read(1 << 20):
            size += len(chunk)
            if size > limit:
                raise HTTPException(status_code=413, detail=f"capture larger than {limit // (1 << 20)} MB")
            out.write(chunk)
    upload.file.seek(0)
    return size


UPLOAD_PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>PacketWhisperer</title>
<style>
 body{font:16px/1.5 system-ui,sans-serif;max-width:42rem;margin:2rem auto;padding:0 1rem;color:#1d2433}
 h1{font-size:1.5rem;margin-bottom:.25rem} p.sub{color:#5b6475;margin-top:0}
 form{display:grid;gap:.75rem;padding:1rem;border:1px solid #d8dde6;border-radius:8px}
 input,textarea,button{font:inherit} button{padding:.5rem 1rem;border:0;border-radius:6px;background:#2b59c3;color:#fff}
 #out{margin-top:1.5rem} .label{font-weight:600;font-size:1.2rem} li{margin:.25rem 0} .muted{color:#5b6475}
</style></head><body>
<h1>PacketWhisperer</h1><p class="sub">Upload a packet capture (.pcap) and get a plain-English explanation.</p>
<form id="f">
 <label>Capture file <input type="file" name="file" accept=".pcap,.pcapng,.cap" required></label>
 <label>What's the problem? (optional)<textarea name="context" rows="2" maxlength="500"></textarea></label>
 <label>API key <input type="password" name="key" autocomplete="off"></label>
 <button>Diagnose</button>
</form>
<div id="out" aria-live="polite"></div>
<script>
const f = document.getElementById('f'), out = document.getElementById('out');
function el(tag, text, cls) {
  const e = document.createElement(tag); e.textContent = text; if (cls) e.className = cls; return e;
}
f.addEventListener('submit', async (ev) => {
  ev.preventDefault();
  out.replaceChildren(el('p', 'Analyzing...', 'muted'));
  const data = new FormData(f); const key = data.get('key'); data.delete('key');
  try {
    const r = await fetch('/v1/diagnose', {method: 'POST', body: data, headers: key ? {'X-API-Key': key} : {}});
    const body = await r.json();
    if (!r.ok) { out.replaceChildren(el('p', 'Error: ' + (body.detail || r.status))); return; }
    const d = body.diagnosis, ul = document.createElement('ul');
    d.evidence.forEach(e => ul.append(el('li', e)));
    const steps = document.createElement('ul'); d.next_steps.forEach(s => steps.append(el('li', s)));
    out.replaceChildren(el('p', d.label.replaceAll('_', ' ') + ' (' + d.confidence + ' confidence)', 'label'),
      el('p', d.summary), el('h3', 'Evidence'), ul, el('h3', 'Next steps'), steps,
      el('p', (d.note ? d.note + ' · ' : '') + 'request ' + body.request_id, 'muted'));
  } catch (e) { out.replaceChildren(el('p', 'Error: ' + e)); }
});
</script></body></html>
"""

app = create_app()
