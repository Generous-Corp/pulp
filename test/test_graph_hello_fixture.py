from pathlib import Path


def test_graph_hello_fixture_is_scoped_and_has_gap_receipt():
    root = Path(__file__).parents[1]
    example = root / "examples" / "graph-hello"
    assert (example / "CMakeLists.txt").is_file()
    assert (example / "main.cpp").is_file()
    doc = (root / "docs" / "guides" / "graph-hello.md").read_text()
    assert "API-gap receipt" in doc
    assert "OfflineSignalGraphHost" in doc
    assert "positive control" in doc
    assert "planted-negative control" in doc
    assert "find_package(Pulp CONFIG REQUIRED COMPONENTS host)" in (
        example / "CMakeLists.txt"
    ).read_text()
