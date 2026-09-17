# Sherlock — tests/Sherlock/QueryHeaderFailure.py
# Verifies that catalog failures keep their diagnostics and cannot produce a misleading header.
import pathlib
import sqlite3
import subprocess
import sys
import tempfile


def create_catalog(path: pathlib.Path, scenario: str) -> str:
    expected_by_scenario = {
        "meta-prepare": ("Mismatch in CheckSchema", "no Sherlock schema"),
        "meta-step": ("Mismatch in CheckSchema", "expected Catalog schema 1"),
        "images": ("Mismatch in CheckSchema", "expected Catalog schema 1"),
    }
    expected = expected_by_scenario[scenario]
    catalog = sqlite3.connect(path)
    try:
        if scenario == "meta-step":
            catalog.execute(
                "CREATE VIEW Meta AS SELECT 'Build' AS Key, abs(-9223372036854775808) AS Value"
            )
        elif scenario == "images":
            catalog.execute("CREATE TABLE Meta (Key TEXT PRIMARY KEY, Value TEXT NOT NULL)")
            catalog.execute("INSERT INTO Meta VALUES ('Build', '26A5416b')")
        catalog.commit()
    finally:
        catalog.close()
    return expected


def main() -> int:
    executable, parent, scenario = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="sherlock-header-", dir=parent) as store:
        expected = create_catalog(pathlib.Path(store) / "Catalog.db", scenario)
        result = subprocess.run(
            [executable, "status", "--store", store], capture_output=True, text=True, check=False
        )
        output = result.stdout + result.stderr
        missing = [fragment for fragment in expected if fragment not in output]
        if (
            result.returncode != 2
            or "verdict: NOT VERIFIED" not in output
            or missing
            or "layer 1 facts" in output
        ):
            sys.stderr.write(output)
            sys.stderr.write(
                f"expected exit 2, diagnostic {expected!r}, and no fabricated header; "
                f"missing {missing!r}, got {result.returncode}\n"
            )
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
