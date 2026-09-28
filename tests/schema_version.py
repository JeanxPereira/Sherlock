# Sherlock — tests/Sherlock/schema_version.py
# The Catalog/Image schema version Python fixtures build against. Mirrors
# tools/Sherlock/Source/Store/include/Store/Schema.h's kSchemaVersion, which is the source of
# truth; this file exists only because a Python fixture cannot #include that header, and every
# fixture that hand-builds a Catalog.db or an Images/<Name>.db imports it rather than writing
# its own literal (rule 2: one value, one place).
STORE_SCHEMA_VERSION = 2
