# Evals

How well does each diagnoser name the root cause? It's measured against
`phase1/casebook/labels.json`: real failures you caused in the lab, with known labels.

```bash
sudo ../../phase1/casebook/run_case.sh all 5          # on the EC2 box: 10 cases × 5 runs
python evals/run_evals.py                              # rules baseline (free)
python evals/run_evals.py --engine rules llm --yes     # head-to-head; ~1 API call per capture
```

Each run writes `results/<timestamp>-<engine>.json` (every prediction) and `.md` (the
table below). The `llm` engine refuses to run without `--yes`, and the report includes
token counts and an estimated cost.

| What to report | Where |
|---|---|
| Overall accuracy | `accuracy` |
| Accuracy per failure type (the resume line) | `per_label` |
| What it confuses with what | `confusion`: e.g. `packet_loss` predicted for `mtu_blackhole` |
| Cost / latency per diagnosis | `estimated_cost_usd`, `mean_latency_s` |

## Making it a fair test
- **Hold some captures out.** If you tune the prompt or the rules while looking at failures, you're fitting to them. Tune on runs 1-3 of each case and report on runs 4-5 (`--labels` with a filtered copy).
- **The LLM has to beat the rules.** If rules score the same on this lab set, that's fine, because the LLM's value shows up on messier real-world captures. Add some of those (from the pilot, with permission) and label them by hand.
- **Watch `healthy`.** A diagnoser that cries wolf is worse than useless for a help desk.

## Smoke test
`python evals/make_synthetic.py && python evals/run_evals.py --labels evals/synthetic/labels.json`
runs the full pipeline on hand-built pcaps. The output is flagged SYNTHETIC. It proves the
plumbing works and says nothing about real accuracy.
