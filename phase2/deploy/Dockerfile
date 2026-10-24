# PacketWhisperer API image. Build from the repo root (it needs phase1/ for the C++ parser):
#   docker build -f phase2/deploy/Dockerfile -t packetwhisperer .

# --- build stage: compile the C++ extension into a wheel
FROM python:3.12-slim-bookworm AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake libpcap-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY phase1/cmake phase1/cmake
COPY phase1/common phase1/common
COPY phase1/dns-resolver phase1/dns-resolver
COPY phase1/mini-tcpdump phase1/mini-tcpdump
COPY phase2/packetwhisperer phase2/packetwhisperer
RUN pip wheel --no-cache-dir --wheel-dir /wheels ./phase2/packetwhisperer

# --- runtime stage: no compilers, non-root
FROM python:3.12-slim-bookworm
RUN apt-get update \
 && apt-get install -y --no-install-recommends libpcap0.8 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=build /wheels /wheels
RUN pip install --no-cache-dir /wheels/*.whl && rm -rf /wheels \
 && useradd --system --uid 10001 pw && mkdir /data && chown pw /data
USER pw
ENV PW_USAGE_LOG=/data/usage.jsonl PYTHONUNBUFFERED=1
EXPOSE 8000
HEALTHCHECK --interval=30s --timeout=3s \
  CMD python -c "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/healthz')"
CMD ["uvicorn", "packetwhisperer.api:app", "--host", "0.0.0.0", "--port", "8000", "--proxy-headers", "--forwarded-allow-ips", "*"]
