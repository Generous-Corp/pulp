# DAWproject import

`import_dawproject_xml` (`core/timeline/import/dawproject_import.cpp`) builds a
`Project` from a DAWproject `project.xml` for the documented linear subset:
single tempo/meter, flat tracks, and beats-timed `<Notes>`/`<Audio>` clips. It
consumes only the model's public construction API and never changes the model
layout.

- Keep foreign-format and heavy-dependency interop in `import/`, not `src/`.
  The web timeline-source-closure gate sweeps every `core/timeline/src/*.cpp`
  into WAM/WebCLAP, which do not link `pulp-runtime`; the importer needs
  pugixml.
- Compile under the timeline library's `-fno-exceptions -fno-rtti` contract.
  Use pugixml DOM traversal, never its throwing XPath API. The runtime XML
  wrapper is too weak to correlate clips with parent lanes, so include pugixml
  privately and reuse the object code already in `pulp::runtime`.
- Fail closed. Unsupported clips, tracks, notes, `<Warps>`, nested group
  tracks, `timeUnit="seconds"`, and unknown timelines produce an error rather
  than being dropped. Skip non-element nodes before matching child tags.
- Seal media identity at the import boundary. Reject rooted, drive-qualified,
  or parent-traversing package paths before calling `DawProjectMediaResolver`.
  Hash returned bytes, inspect WAV metadata, require it to match the XML, and
  then retain the safe path as a `PackageRelative` hint. Deduplicate by sealed
  content hash while preserving distinct safe locators.
- Apply `DawProjectImportLimits` before constructing the XML DOM or growing
  structural, locator, asset, or media-processing state. A legacy resolver owns
  its returned-vector allocation, so untrusted package readers must enforce the
  same per-call byte ceiling while reading the entry.
