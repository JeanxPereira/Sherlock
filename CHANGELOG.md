# Sherlock changelog

Each version states what it adds and the measurement behind every performance claim.

## 0.1.0

- Foundation: `Diagnostic`, `Expected`, `ByteReader`, `MappedFile`.
- Store: owning SQLite wrappers, schema version 1 for the catalog and the image stores.
- DyldSharedCache: the split cache as one address space, slide info v5 pointers and rebase chains.
- MachO: segments, sections, function starts and symbol table of one image; DyldSharedCache gains Owner and FindImage.
- Facts: capstone-backed extraction of functions, calls (direct/island/unresolved), literal reads and coverage; WriteImageFacts round-trips into an image store.
