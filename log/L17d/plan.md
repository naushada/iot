# L17d Plan — rate-limit + chaos coverage

> **Status (2026-06-03):** **CLOSED.** All shipped in one commit.

Two hardening features for the services control plane:

1. **Rate-limit** — ds-server rejects sets on the same key within a
   configurable window (default 0 = disabled). Prevents operator
   churn from flooding daemons with spawn/reap cycles.

2. **Chaos harness** — shell script that randomly flips every
   `services.*.enable` gate and asserts daemons stay alive.

## Design

### Rate-limit
- `DataStore::m_last_set` map tracks last-set timestamp per key
- `DataStore::set_rate_limit_ms(N)` enables the window
- `DataStore::is_rate_limited(key)` checks if key was set within N ms
- Worker Set handler calls `is_rate_limited()` before touching store
- New proto status `RateLimited = 0x8007`
- `ds-server rate-limit-ms=1000` CLI arg (default 0 = off)

### Chaos harness
- `log/L17d/chaos.sh` — picks random service, disables, sleeps
  random interval, re-enables, checks daemon liveness
- Configurable cycles, min/max sleep via env vars
- Counts successes, rate-limited hits, and failures

## Usage

```sh
# Start ds-server with 1-second rate limit
ds-server rate-limit-ms=1000

# Run chaos (50 cycles default)
bash log/L17d/chaos.sh
```

## Non-goals (v1)
- Adaptive rate-limit (backoff on repeated violations)
- Per-key rate-limit windows (one global window)
- Chaos harness integration into CI (manual run for now)
