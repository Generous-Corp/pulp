# browser-capture-text-stale-face

`browser-capture-text-wrap` with **`platform-fonts.json` removed**, and nothing
else changed. That single missing file is the exact shape of a snapshot taken
before the capture collected resolved font faces.

It exists because the consequence is invisible. The runs still carry their
captured line boxes, so the capture looks complete — but the renderer refuses a
basis with no face on purpose (a font family is a *request*, and without the
face the text was broken against there is no way to know the cache still
describes this machine). Every run then silently re-derives its own line
breaking, and a run that resumes mid-line after an inline `<span>` loses the
horizontal offset that placed it and prints on top of its own sibling.

That was not hypothetical: the delay panel's capture was in this state and its
annotation column rendered two runs of text overprinted on one line. Nothing in
the output said the capture was the cause, which is what this fixture and the
`native_text_stale_capture` signal exist to prevent.

Every other sidecar is copied verbatim, including `browser.png`, which the
loader requires — the point is that removing ONE file changes the lowering's
verdict, so any other difference between the two fixtures would muddy what the
test is attributing the change to.
