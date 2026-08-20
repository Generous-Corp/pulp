// Consumer metadata commonly comes from generated package manifests. This
// module owns the demo shell's DOM boundary: metadata is text, and links resolve
// only to ordinary web navigation.

// play.svg from the shipped Ink & Signal icon set (Lucide-derived, ISC — see
// NOTICE.md / DEPENDENCIES.md). The translate optically centers the triangle.
const PLAY_SVG = `<svg viewBox="0 0 24 24" width="26" height="26" aria-hidden="true" focusable="false"><g transform="translate(-0.6 0)"><path d="M8 6.1v11.8a1 1 0 0 0 1.53.85l9.3-5.9a1 1 0 0 0 0-1.7L9.53 5.25A1 1 0 0 0 8 6.1Z" fill="currentColor"/></g></svg>`;

function safeWebHref(value) {
  if (typeof value !== "string") return null;
  try {
    const parsed = new URL(value, document.baseURI || "https://pulp.invalid/");
    return parsed.protocol === "http:" || parsed.protocol === "https:" ? value : null;
  } catch {
    return null;
  }
}

function externalLink(label, href) {
  const link = document.createElement("a");
  link.setAttribute("href", href);
  link.setAttribute("target", "_blank");
  link.setAttribute("rel", "noopener");
  link.textContent = label;
  return link;
}

export function buildShellDom({
  root, title, subtitle, galleryHref, sourceHref, hostLabel, hostDocsHref, startWord,
}) {
  // Package-owned markup only. Consumer values are attached below through DOM
  // text/attribute APIs and cannot change the tree shape.
  root.innerHTML = `
    <div class="pp-top">
      <a id="pp-gallery">&larr; Gallery</a>
      <span>Pulp <span id="pp-host"></span> demo<span id="pp-source"></span></span>
    </div>
    <div id="panel" class="pulp">
      <h1></h1>
      <div class="sub"></div>
      <div id="params"></div>
      <div id="fileup"></div>
      <div id="body"></div>
      <div id="footer">
        <button id="stop">Stop Audio</button>
        <div id="status"></div>
      </div>
      <div id="overlay" role="dialog">
        <h2 id="ov-name"></h2>
        <div id="ov-start" role="button" tabindex="0" aria-label="Start audio"></div>
        <div id="ov-hint"></div>
      </div>
    </div>`;

  const $ = (selector) => root.querySelector(selector);
  $("#pp-gallery").setAttribute("href", safeWebHref(galleryHref) || "../index.html");

  const safeHostDocsHref = safeWebHref(hostDocsHref);
  if (safeHostDocsHref) $("#pp-host").appendChild(externalLink(hostLabel, safeHostDocsHref));
  else $("#pp-host").textContent = hostLabel;

  const safeSourceHref = safeWebHref(sourceHref);
  if (safeSourceHref) {
    $("#pp-source").textContent = " · ";
    $("#pp-source").appendChild(externalLink("source", safeSourceHref));
  }

  $("h1").textContent = title;
  $(".sub").textContent = subtitle;
  $("#overlay").setAttribute("aria-label", `Start ${title}`);
  $("#ov-name").textContent = title;
  if (subtitle) {
    const description = document.createElement("p");
    description.id = "ov-desc";
    description.textContent = subtitle;
    $("#ov-name").after(description);
  }
  $("#ov-start").innerHTML = PLAY_SVG;
  $("#ov-hint").textContent = startWord;
  document.title = `${title} — Pulp web demo`;
  return $;
}
