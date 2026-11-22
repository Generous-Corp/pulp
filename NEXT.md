# CHOC Windows investigation

The regression test `a patched shared source updates FetchContent's cache path` was added to `tools/scripts/test_setup_source_cache.sh` before the fix. On `origin/main` it fails with:

```
FetchContent cache consumers use the patched build-local source (want '<build>/_deps/pulp-mutable-choc-src', got '<cache>/choc-v1>')
```

The full script also reports pre-existing fixture failures in this lane. The regression failure is independent and reproducible with the inline CMake fixture.
