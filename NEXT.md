# CHOC Windows investigation

The regression test `a patched shared source updates FetchContent's cache path` was added to `tools/scripts/test_setup_source_cache.sh` before the fix. On `origin/main` it fails with:

```
FetchContent cache consumers use the patched build-local source (want '<build>/_deps/pulp-mutable-choc-src', got '<cache>/choc-v1>')
```

The full script also reports pre-existing fixture failures in this lane. The regression failure is independent and reproducible with the inline CMake fixture.

The fix in `ecfdf6f6c8` forces `FETCHCONTENT_SOURCE_DIR_CHOC` to the same build-local path in the cache while retaining the normal variable. With the fix, the regression passes. Reverting only that fix reproduced the failure, then the file was restored from `ecfdf6f6c8` without leaving a working-tree change.

The downloaded Windows closure identifies `pulp-events` and `pulp-runtime` as the affected targets. Their CHOC include directory arrives through `core/runtime/CMakeLists.txt` and resolves to the stale FetchContent cache path. No Windows-only `$CACHE{}` or direct `FETCHCONTENT_SOURCE_DIR_CHOC` consumer exists in the repository. The stale cache entry is the shared-path source of the target property.
