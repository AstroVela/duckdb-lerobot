# DuckDB test-host patches

Native CI applies `duckdb-1.5.5-cached-eof.patch` to the pinned DuckDB commit
`d8cdaa33fda8df955cc76ef58a280f68f4cd43fa` before compiling all three test hosts.
The patch does not change the submodule revision or the distributed extension.
The distribution pipeline continues to build against the unmodified baseline.
After the Native CI tests finish, the workflow reverses this exact patch before
checking repository cleanliness; any other source changes still fail that check.

DuckDB 1.5.5's JSON format detection performs a sequential read at EOF. With
the external file cache disabled, `CachingFileSystemWrapper` obtains a valid
zero-sized buffer handle with a null data pointer and passes that pointer to
`memcpy` with a zero length. UBSAN's nonnull-argument check rejects this call.
The patch returns before allocating or copying a zero-byte read. It does not
disable sanitizer checks or change nonempty reads.

The existing `cache_scope` C++ cases reproduce the failure with an unmodified,
fully instrumented DuckDB host. They still run with ASAN, UBSAN and LSan enabled.
Reassess and remove this patch when updating the DuckDB baseline to a revision
that handles zero-byte cached reads itself.
