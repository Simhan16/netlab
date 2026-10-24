from __future__ import annotations

from typing import Literal

from pydantic import BaseModel, Field, field_validator

from packetwhisperer.taxonomy import LABELS

Confidence = Literal["low", "medium", "high"]


class Usage(BaseModel):
    input_tokens: int = 0
    output_tokens: int = 0
    model: str | None = None


class Diagnosis(BaseModel):
    label: str
    confidence: Confidence
    summary: str = Field(description="Plain-English explanation for a non-engineer")
    evidence: list[str] = Field(default_factory=list, description="Specific facts from the capture")
    next_steps: list[str] = Field(default_factory=list)
    engine: str = "rules"
    note: str | None = None  # e.g. "LLM unavailable, used rules"
    usage: Usage | None = None

    @field_validator("label")
    @classmethod
    def known_label(cls, value: str) -> str:
        if value not in LABELS:
            raise ValueError(f"unknown label {value!r}")
        return value
