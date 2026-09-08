Save a minimized libFuzzer reproducer here with a descriptive `.bin` filename
after fixing its defect. `make_corpus.py` includes every such file in the
mandatory seed replay. Keep a focused SQL or C++ regression too when the
failure exposes an observable API contract.

Inputs have three header bytes: mode (modulo 4), option/schema index, scalar
type (modulo 6). Remaining bytes are an arbitrary string, including embedded
NULs. Modes are paths, COPY options, COPY FEATURES JSON, and video/temporal
options. Scalar types are NULL, BIGINT, UBIGINT, DOUBLE, BOOLEAN and VARCHAR.
The exact option indexes and schema variants are in `validation_fuzz.cpp`.
For video options, the low seven index bits select the option; the high bit
also sets the peer dimension when fuzzing width or height.

`oversized-array-dimension.bin` is a reduced FEATURES JSON input with an array
dimension of 288078. It previously reached DuckDB's `MAX_ARRAY_SIZE` assertion
during COPY binding; it must now raise a normal input error. The COPY binding
suite also checks the 100000-element boundary, nested shapes, and connection
cleanup and reuse after rejection.
