# AETHER — investor pitch deck

A [reveal.js](https://revealjs.com) slide deck for the device + cloud IoT
platform (secure device management · multi-protocol telemetry · A/B OTA ·
**edge container runtime**).

## Files

| File | What |
|------|------|
| `aether-pitch.md` | The deck content — **single source of truth**. Reads fine on its own (GitHub renders it); reveal.js splits it into slides on `---`. |
| `index.html` | reveal.js shell that loads `aether-pitch.md` (CDN — no install). |

## View it

The reveal shell loads the markdown via `fetch`, so it needs to be served over
http (opening `index.html` from `file://` will be blocked by the browser). From
this folder:

```sh
cd pitch
python3 -m http.server 8000
# then open http://localhost:8000/  (press 'S' for speaker notes)
```

Keys: `→/Space` next · `←` back · `F` fullscreen · `S` speaker notes · `Esc` overview.

## Edit

Edit only `aether-pitch.md`. Slides are separated by a line containing just
`---`; speaker notes start a line with `Note:`. No build step.

## Export to PDF

Append `?print-pdf` to the URL and use the browser's "Print → Save as PDF"
(landscape, background graphics on):

```
http://localhost:8000/?print-pdf
```
