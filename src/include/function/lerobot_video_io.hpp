//===----------------------------------------------------------------------===//
//                         DuckDB
//
// function/lerobot_video_io.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#ifdef LEROBOT_HAVE_FFMPEG

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"

struct AVFormatContext;
struct AVIOContext;

namespace duckdb {

class FileSystem;
class ClientContext;

// Owns every AVIO stream for one file, including MP4 faststart's read handle.
// FFmpeg sees a fixed label; only DuckDB interprets the actual filesystem path.
class LerobotVideoIO {
public:
	// Borrowed cancellation sources must outlive this adapter and its handles.
	LerobotVideoIO(FileSystem &fs, string path, bool writable, optional_ptr<atomic<bool>> cancelled = nullptr,
	               optional_ptr<ClientContext> context = nullptr);
	~LerobotVideoIO();

	static const char *URL();
	void Open(AVFormatContext &format);
	// Flush, sync and close, recording errors without throwing from C callbacks
	// or destructors. The successful operation must then call ThrowIfError().
	void Close(AVIOContext *&context) noexcept;
	void ThrowIfError() const;

private:
	struct State;
	unique_ptr<State> state;
};

} // namespace duckdb

#endif
