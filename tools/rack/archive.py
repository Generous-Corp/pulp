#!/usr/bin/env python3
"""Reading and writing .vcvplugin archives, on a Mac that has no `zstd`.

A .vcvplugin is a zstd-compressed tar, and this product shelled out to the
`zstd` binary in three places. macOS does not ship one: there is no
/usr/bin/zstd on 26.x, none in the Command Line Tools, and none in Xcode. It
arrives with Homebrew, which every development machine has and no beta user
does -- and even with Homebrew installed, /opt/homebrew/bin is not on the PATH
an app launched from Finder inherits.

The failures it caused did not look like a missing tool:

  - reading a freshly installed plugin fell into `except: pass`, so the plugin
    read as NOT INSTALLED and the lint then rejected the very patch built to
    use it
  - packaging a generated module ran with check=True and died on
    FileNotFoundError at the last step, after the model call, the compile and
    the gate had all succeeded

macOS DOES ship libarchive's bsdtar at /usr/bin/tar, which reads and writes
zstd with `--zstd` and is byte-compatible with the real thing in both
directions (proven: an archive from one is read by the other). So the
dependency is removed rather than documented. `zstd` is still preferred when
it is there, because it is what the format's own tooling uses.
"""
from __future__ import annotations

import os
import shutil
import signal
import stat
import subprocess
import tempfile

# /usr/bin/tar is BSD tar (libarchive) on macOS and is always present. Named
# absolutely rather than looked up, because the whole point is to work in a
# process whose PATH holds almost nothing.
SYSTEM_TAR = "/usr/bin/tar"


def _tar() -> str:
    if os.path.exists(SYSTEM_TAR):
        return SYSTEM_TAR
    found = shutil.which("tar")
    if not found:
        raise SystemExit("no `tar` on this machine, so plugin archives cannot "
                         "be read or written.")
    return found


def _zstd() -> str | None:
    """The zstd binary, searched the way an app-launched process must.

    shutil.which alone consults the inherited PATH, which for a Finder launch
    is /usr/bin:/bin:/usr/sbin:/sbin -- so a machine WITH zstd reported not
    having it.
    """
    try:
        import toolpaths
        return shutil.which("zstd", path=toolpaths.enriched_path())
    except Exception:                                           # noqa: BLE001
        return shutil.which("zstd")


def read_member(archive: str, basename: str) -> bytes | None:
    """The first member whose file name is `basename`, or None.

    Listed and then extracted by exact name rather than by glob: bsdtar's
    pattern matching and tarfile's are not the same language, and a helper
    that behaves differently depending on which tool is installed is worse
    than one that is simply missing.
    """
    zstd = _zstd()
    if zstd:
        import io
        import tarfile
        try:
            raw = subprocess.run([zstd, "-dc", archive], capture_output=True,
                                 timeout=60).stdout
            with tarfile.open(fileobj=io.BytesIO(raw)) as tf:
                for member in tf.getmembers():
                    if os.path.basename(member.name) == basename:
                        fh = tf.extractfile(member)
                        return fh.read() if fh else None
        except Exception:                                       # noqa: BLE001
            return None
        return None

    try:
        listing = subprocess.run([_tar(), "--zstd", "-tf", archive],
                                 capture_output=True, timeout=60, text=True)
        if listing.returncode != 0:
            return None
        for name in listing.stdout.splitlines():
            if os.path.basename(name.rstrip("/")) != basename:
                continue
            got = subprocess.run([_tar(), "--zstd", "-xOf", archive, name],
                                 capture_output=True, timeout=60)
            return got.stdout if got.returncode == 0 else None
    except Exception:                                           # noqa: BLE001
        return None
    return None


def _extract_all_into(archive: str, dest_dir: str) -> bool:
    """Unpack directly into a private staging directory."""
    zstd = _zstd()
    try:
        if zstd:
            decoded = subprocess.run([zstd, "-dc", archive],
                                     capture_output=True, timeout=60)
            if decoded.returncode != 0:
                return False
            listing = subprocess.run([_tar(), "-tf", "-"],
                                     input=decoded.stdout, capture_output=True,
                                     timeout=60, text=False)
            listing_text = listing.stdout.decode("utf-8", errors="strict")
        else:
            listing = subprocess.run([_tar(), "--zstd", "-tf", archive],
                                     capture_output=True, timeout=60, text=True)
            listing_text = listing.stdout
    except Exception:                                           # noqa: BLE001
        return False
    if listing.returncode != 0:
        return False
    for name in listing_text.splitlines():
        normalized = name.rstrip("/")
        parts = normalized.split("/")
        if (not normalized or os.path.isabs(normalized) or
                any(part in ("", "..") for part in parts)):
            return False

    producer = None
    try:
        if zstd:
            producer = subprocess.Popen([zstd, "-dc", archive],
                                        stdout=subprocess.PIPE)
            consumer = subprocess.run(
                [_tar(), "xf", "-"], stdin=producer.stdout,
                cwd=dest_dir, timeout=120)
            if producer.stdout:
                producer.stdout.close()
            producer_status = producer.wait(timeout=10)
            # bsdtar stops at the tar end marker and may close its input before
            # zstd has written frame padding/checksum bytes. That normal close
            # appears to zstd as SIGPIPE even though tar validated and unpacked
            # the complete archive.
            producer_ok = producer_status in (0, -signal.SIGPIPE)
            return producer_ok and consumer.returncode == 0
        r = subprocess.run([_tar(), "--zstd", "-xf", archive, "-C", dest_dir],
                           timeout=120)
        return r.returncode == 0
    except Exception:                                           # noqa: BLE001
        return False
    finally:
        if producer is not None:
            if producer.stdout:
                producer.stdout.close()
            if producer.poll() is None:
                producer.terminate()
                try:
                    producer.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    producer.kill()
                    producer.wait()


def extract_all(archive: str, dest_dir: str) -> bool:
    """Atomically publish the single plug-in directory from an archive."""
    if not os.path.isdir(dest_dir):
        return False
    stage = tempfile.mkdtemp(prefix=".forge-extract-", dir=dest_dir)
    try:
        if not _extract_all_into(archive, stage):
            return False
        entries = os.listdir(stage)
        if len(entries) != 1:
            return False
        staged_plugin = os.path.join(stage, entries[0])
        try:
            top_mode = os.lstat(staged_plugin).st_mode
        except OSError:
            return False
        if not stat.S_ISDIR(top_mode):
            return False
        # A plug-in archive is data plus regular directories. Publishing a
        # symlink would let an archive redirect later reads outside the
        # private extraction root, including after this function returns.
        for root, dirs, files in os.walk(staged_plugin, followlinks=False):
            for name in dirs + files:
                try:
                    mode = os.lstat(os.path.join(root, name)).st_mode
                except OSError:
                    return False
                if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
                    return False
        published_plugin = os.path.join(dest_dir, entries[0])
        # lexists() also sees dangling links. os.path.exists() does not, and
        # os.rename() would replace that directory entry.
        if os.path.lexists(published_plugin):
            return False
        os.rename(staged_plugin, published_plugin)
        return True
    except OSError:
        return False
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def create(archive: str, root_dir: str, member: str) -> None:
    """Write `member` (a directory under root_dir) as a .vcvplugin.

    --no-xattrs because macOS attaches resource forks that appear in the
    archive as ._ files, which Rack then tries to read as part of the plugin.
    """
    zstd = _zstd()
    if zstd:
        producer = None
        try:
            producer = subprocess.Popen(
                [_tar(), "--no-xattrs", "-cf", "-", "-C", root_dir, member],
                stdout=subprocess.PIPE)
            consumer = subprocess.run(
                [zstd, "-q", "-19", "-o", archive], stdin=producer.stdout)
            # Close our duplicate read end before waiting. If zstd quit early,
            # tar must see the broken pipe rather than blocking on a reader
            # that will never consume another byte.
            if producer.stdout:
                producer.stdout.close()
            producer_status = producer.wait()
            if producer_status != 0:
                raise subprocess.CalledProcessError(producer_status,
                                                    producer.args)
            if consumer.returncode != 0:
                raise subprocess.CalledProcessError(consumer.returncode,
                                                    consumer.args)
        except Exception:
            if producer is not None and producer.poll() is None:
                if producer.stdout:
                    producer.stdout.close()
                producer.terminate()
                producer.wait()
            try:
                os.remove(archive)
            except FileNotFoundError:
                pass
            raise
        return
    try:
        subprocess.run(
            [_tar(), "--no-xattrs", "--zstd",
             "--options", "zstd:compression-level=19",
             "-cf", archive, "-C", root_dir, member],
            check=True)
    except Exception:
        try:
            os.remove(archive)
        except FileNotFoundError:
            pass
        raise
