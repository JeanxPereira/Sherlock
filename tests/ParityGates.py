#!/usr/bin/env python3
"""Focused regressions for parity decisions that need no Apple corpus."""
from __future__ import annotations

import contextlib
import io
import os
import sqlite3
import subprocess
import sys
import uuid
from pathlib import Path
from unittest import TestCase, main as unittest_main
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import Parity  # noqa: E402


class FakeSymbols:
    def __init__(self, _cache_dir: Path) -> None:
        pass

    def _load(self, _path: str) -> dict[str, int]:
        return {"_$sSelectedOnlyByPython": 0x1234}


class ParityGates(TestCase):
    def setUp(self) -> None:
        scratch = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "ParityGateScratch"
        self.work = scratch / uuid.uuid4().hex
        self.work.mkdir(parents=True)
        self.store = self.work / "store"
        (self.store / "Images").mkdir(parents=True)
        self.catalog_path = self.store / "Catalog.db"

    def catalog(self) -> sqlite3.Connection:
        db = sqlite3.connect(self.catalog_path)
        db.execute("CREATE TABLE Image (Path TEXT NOT NULL, Name TEXT NOT NULL)")
        return db

    def test_python_selected_symbol_missing_from_sherlock_fails(self) -> None:
        catalog = self.catalog()
        catalog.execute("INSERT INTO Image VALUES (?, ?)", (Parity.SWIFTUICORE, "SwiftUICore.db"))
        catalog.commit()
        image = sqlite3.connect(self.store / "Images" / "SwiftUICore.db")
        image.executescript(
            "CREATE TABLE Name (Id INTEGER PRIMARY KEY, Text TEXT NOT NULL);"
            "CREATE TABLE Symbol (Name INTEGER NOT NULL, Address INTEGER NOT NULL);"
        )
        image.commit()
        output = io.StringIO()
        with patch.object(Parity.symbols, "Symbols", FakeSymbols), contextlib.redirect_stdout(output):
            result = Parity.check_symbols(self.work, self.store, catalog)
        self.assertFalse(result)
        self.assertIn("FAIL symbols", output.getvalue())
        self.assertIn("ipsw-only 1", output.getvalue())
        catalog.close()
        image.close()

    def test_image_db_does_not_create_missing_database(self) -> None:
        catalog = self.catalog()
        catalog.execute("INSERT INTO Image VALUES (?, ?)", (Parity.DESIGN_LIBRARY, "Missing.db"))
        catalog.commit()
        missing = self.store / "Images" / "Missing.db"
        with self.assertRaises(RuntimeError):
            Parity.image_db(self.store, catalog, Parity.DESIGN_LIBRARY)
        self.assertFalse(missing.exists())
        catalog.close()

    def test_image_db_is_read_only(self) -> None:
        catalog = self.catalog()
        catalog.execute("INSERT INTO Image VALUES (?, ?)", (Parity.DESIGN_LIBRARY, "Existing.db"))
        catalog.commit()
        path = self.store / "Images" / "Existing.db"
        image = sqlite3.connect(path)
        image.execute("CREATE TABLE Existing (Value INTEGER)")
        image.commit()
        image.close()
        image = Parity.image_db(self.store, catalog, Parity.DESIGN_LIBRARY)
        with self.assertRaises(sqlite3.OperationalError):
            image.execute("CREATE TABLE Mutation (Value INTEGER)")
        image.close()
        image = sqlite3.connect(path)
        self.assertIsNone(
            image.execute("SELECT name FROM sqlite_master WHERE name = 'Mutation'").fetchone()
        )
        image.close()
        catalog.close()

    def run_cli(self) -> tuple[int, str]:
        env = dict(os.environ)
        env["AQUAKIT_REFS"] = str(self.work / "no-corpus")
        proc = subprocess.run(
            [sys.executable, str(Parity.__file__), "--sherlock", str(self.work / "Sherlock.exe"),
             "--store", str(self.store)],
            capture_output=True,
            text=True,
            env=env,
        )
        return proc.returncode, proc.stdout

    def test_main_missing_catalog_row_is_not_verified(self) -> None:
        catalog = self.catalog()
        catalog.commit()
        catalog.close()
        result, output = self.run_cli()
        self.assertEqual(result, 2)
        self.assertIn("NOT VERIFIED", output)
        self.assertIn("no Image row", output)

    def test_main_missing_image_database_is_not_verified_without_creation(self) -> None:
        catalog = self.catalog()
        catalog.execute("INSERT INTO Image VALUES (?, ?)", (Parity.DESIGN_LIBRARY, "Absent.db"))
        catalog.commit()
        catalog.close()
        missing = self.store / "Images" / "Absent.db"
        result, output = self.run_cli()
        self.assertEqual(result, 2)
        self.assertIn("NOT VERIFIED", output)
        self.assertIn("missing image database", output)
        self.assertFalse(missing.exists())


if __name__ == "__main__":
    unittest_main(argv=[sys.argv[0]], verbosity=2)
