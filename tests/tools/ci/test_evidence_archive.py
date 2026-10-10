import json, subprocess, sys
from pathlib import Path
SCRIPT = Path(__file__).parents[3] / "tools/ci/evidence_archive.py"
def run(tmp_path, data, sessions=None):
    fixture = tmp_path / "github.json"; fixture.write_text(json.dumps(data))
    cmd=[sys.executable,str(SCRIPT),"--root",str(tmp_path/"archive"),"--github-json",str(fixture),"--now","2026-10-10T00:00:00Z"]
    if sessions: cmd += ["--sessions-root",str(sessions)]
    return subprocess.run(cmd,check=True,capture_output=True,text=True)
def test_redacts_token_email_key_and_is_idempotent(tmp_path):
    data={"runs":[{"id":7,"head_sha":"abc","event":"push","failure_evidence":"ghp_SECRET12345678 user@example.com api_key=live-key"}]}
    first=run(tmp_path,data); second=run(tmp_path,data)
    assert json.loads(first.stdout)["counts"]["ci_job"] == 1
    assert json.loads(second.stdout)["counts"]["skipped_duplicate"] == 1
    blob="".join(p.read_text() for p in (tmp_path/"archive").glob("*.jsonl"))
    assert "ghp_SECRET" not in blob and "user@example.com" not in blob and "live-key" not in blob
    assert "REDACTED" in blob
def test_session_retention_is_shorter(tmp_path):
    sessions=tmp_path/"sessions"; sessions.mkdir(); (sessions/"x.jsonl").write_text(json.dumps({"timestamp":"2026-09-01T00:00:00Z","error":"old"})+"\n")
    result=run(tmp_path,{"runs":[]},sessions)
    assert json.loads(result.stdout)["counts"]["skipped_retention"] == 1
