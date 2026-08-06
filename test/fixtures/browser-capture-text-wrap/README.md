# Browser capture fixture — text wrapping and weight

A real Chrome capture of `panel.html`, recorded so tests can assert against
Chrome's own line boxes and advances rather than against numbers computed a
second time by the code under test.

## What each run is for

| element | what it pins |
|---|---|
| `#wrapped` | Five line boxes from one text run. Four of the five are **narrower** than the run's layout box, so reading `layout.bounds` instead of `textBoxes` is wrong on four of five — a fixture where they agree cannot tell the two apart. |
| `#unwrapped` | The same text with `white-space: nowrap`. One box, **wider** than its 220 px block, so "the box is the CSS width" is wrong here too. |
| `#regular` / `#bold` | Identical text, identical 20 px size, one weight apart on a variable font with real 400 and 700 instances. The width difference between their boxes is Chrome's own answer to what weight costs. |

## Fonts

`panel.html` loads `Inter-Regular.ttf` and
`FunnelDisplay-VariableFont_wght.ttf` by `@font-face` from beside itself, so
Chrome shapes the same binaries the repo ships and the recorded advances do not
depend on what the capturing machine had installed. The two files are **not**
committed here — they are copied in at capture time from `external/fonts/`, and
the recorded artifacts are what tests read.

## Regenerating

```sh
STAGE=$(mktemp -d)
cp external/fonts/Inter-Regular.ttf \
   external/fonts/FunnelDisplay-VariableFont_wght.ttf \
   test/fixtures/browser-capture-text-wrap/panel.html "$STAGE"/
node tools/import-design/browser_capture/capture.mjs capture \
  --browser "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
  --input "$STAGE/panel.html" --root "$STAGE" \
  --output test/fixtures/browser-capture-text-wrap \
  --initial-width 640 --initial-height 360 --dpr 2 --timeout-ms 120000
```

Regenerating moves every recorded number. A Chrome version change legitimately
shifts advances by fractions of a pixel; a change that moves them by more than
that is a finding about the capture, not a fixture to refresh past.
