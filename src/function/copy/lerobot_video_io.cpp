#include "function/lerobot_video_io.hpp"

#ifdef LEROBOT_HAVE_FFMPEG

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/main/client_context.hpp"

#include <cerrno>
#include <cstring>
#include <exception>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}

namespace duckdb {

struct LerobotVideoIO::State {
	struct Handle {
		explicit Handle(State &owner_p) : owner(owner_p) {
		}
		~Handle() {
			if (context) {
				if (context->write_flag) {
					avio_flush(context);
					try {
						if (context->error < 0 && !owner.error) {
							throw IOException("Failed to flush LeRobot video '%s' (FFmpeg error %d)", owner.path,
							                  context->error);
						}
						if (!owner.error) {
							file->Sync();
						}
					} catch (...) {
						owner.RecordError();
					}
				}
				// FFmpeg may have replaced the original allocation while reading.
				av_freep(&context->buffer);
				avio_context_free(&context);
			}
			if (file) {
				try {
					file->Close();
				} catch (...) {
					owner.RecordError();
				}
			}
		}

		State &owner;
		unique_ptr<FileHandle> file;
		AVIOContext *context = nullptr;
		int64_t position = 0;
	};

	State(FileSystem &fs_p, string path_p, bool writable_p, optional_ptr<atomic<bool>> cancelled_p,
	      optional_ptr<ClientContext> context_p)
	    : fs(fs_p), path(std::move(path_p)), writable(writable_p), cancelled(cancelled_p), context(context_p) {
	}
	~State() {
		// Also covers avformat_open_input failures: CUSTOM_IO remains ours even
		// when FFmpeg destroys the AVFormatContext before returning the error.
		while (!handles.empty()) {
			Close(handles.begin()->first);
		}
	}

	void RecordError() noexcept {
		if (!error) {
			error = std::current_exception();
		}
	}
	void Check() const {
		if (error) {
			std::rethrow_exception(error);
		}
		if ((cancelled && cancelled->load()) || (context && context->IsInterrupted())) {
			throw InterruptException();
		}
	}

	static int Read(void *opaque, uint8_t *buffer, int size) noexcept {
		auto &handle = *static_cast<Handle *>(opaque);
		try {
			handle.owner.Check();
			if (size <= 0) {
				return AVERROR(EINVAL);
			}
			const auto count = handle.file->Read(buffer, size);
			if (count < 0 || count > size || handle.position > NumericLimits<int64_t>::Maximum() - count) {
				throw IOException("Invalid read size for LeRobot video '%s'", handle.owner.path);
			}
			handle.position += count;
			return count == 0 ? AVERROR_EOF : static_cast<int>(count);
		} catch (...) {
			handle.owner.RecordError();
			return AVERROR(EIO);
		}
	}

#if LIBAVFORMAT_VERSION_MAJOR >= 61
	static int Write(void *opaque, const uint8_t *buffer, int size) noexcept {
#else
	static int Write(void *opaque, uint8_t *buffer, int size) noexcept {
#endif
		auto &handle = *static_cast<Handle *>(opaque);
		try {
			handle.owner.Check();
			if (size < 0 || handle.position > NumericLimits<int64_t>::Maximum() - size) {
				throw IOException("Invalid write size for LeRobot video '%s'", handle.owner.path);
			}
			const auto count = handle.file->Write(const_cast<uint8_t *>(buffer), size);
			if (count != size) {
				throw IOException("Failed to write complete LeRobot video data to '%s': wrote %lld of %d bytes",
				                  handle.owner.path, count, size);
			}
			handle.position += count;
			return size;
		} catch (...) {
			handle.owner.RecordError();
			return AVERROR(EIO);
		}
	}

	static int64_t FileSize(Handle &handle) {
		// FileHandle::GetFileSize performs an internal checked unsigned cast;
		// an unsupported/invalid size from a filesystem must be an I/O error.
		const auto size = handle.file->file_system.GetFileSize(*handle.file);
		if (size < 0) {
			throw IOException("Invalid file size for LeRobot video '%s'", handle.owner.path);
		}
		return size;
	}

	static int64_t Seek(void *opaque, int64_t offset, int whence) noexcept {
		auto &handle = *static_cast<Handle *>(opaque);
		try {
			handle.owner.Check();
			whence &= ~AVSEEK_FORCE;
			if (whence == AVSEEK_SIZE) {
				return FileSize(handle);
			}
			int64_t base = 0;
			if (whence == SEEK_CUR) {
				base = handle.position;
			} else if (whence == SEEK_END) {
				base = FileSize(handle);
			} else if (whence != SEEK_SET) {
				return AVERROR(EINVAL);
			}
			if (offset < -base || (offset > 0 && base > NumericLimits<int64_t>::Maximum() - offset)) {
				return AVERROR(EINVAL);
			}
			handle.file->Seek(static_cast<idx_t>(base + offset));
			handle.position = base + offset;
			return handle.position;
		} catch (...) {
			handle.owner.RecordError();
			return AVERROR(EIO);
		}
	}

	AVIOContext *OpenHandle(bool write) {
		Check();
		auto handle = make_uniq<Handle>(*this);
		auto flags = write ? FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW |
		                         FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE
		                   : FileFlags::FILE_FLAGS_READ;
		handle->file = fs.OpenFile(path, flags);
		if (!handle->file->CanSeek()) {
			throw IOException("LeRobot MP4 requires a seekable file: '%s'", path);
		}
		const int buffer_size = 64 * 1024;
		auto buffer = static_cast<uint8_t *>(av_malloc(buffer_size));
		if (!buffer) {
			throw OutOfMemoryException("Failed to allocate LeRobot video I/O buffer");
		}
		handle->context = avio_alloc_context(buffer, buffer_size, write, handle.get(), write ? nullptr : Read,
		                                     write ? Write : nullptr, Seek);
		if (!handle->context) {
			av_free(buffer);
			throw OutOfMemoryException("Failed to allocate LeRobot video I/O context");
		}
		auto result = handle->context;
		handles.emplace(result, std::move(handle));
		return result;
	}

	static int OpenCallback(AVFormatContext *format, AVIOContext **context, const char *url, int flags,
	                        AVDictionary **) noexcept {
		auto &owner = *static_cast<State *>(format->opaque);
		*context = nullptr;
		try {
			// The only secondary access is MP4 faststart reopening its output.
			// Never fall through to FFmpeg protocols or parse a filesystem path.
			if (!url || strcmp(url, LerobotVideoIO::URL()) != 0 || flags != AVIO_FLAG_READ) {
				throw IOException("Unexpected FFmpeg I/O request while writing LeRobot video '%s'", owner.path);
			}
			*context = owner.OpenHandle(false);
			return 0;
		} catch (...) {
			owner.RecordError();
			return AVERROR(EIO);
		}
	}
	void Close(AVIOContext *context) noexcept {
		// Remove the registration before flushing/closing its owned resources.
		auto entry = handles.find(context);
		if (entry != handles.end()) {
			auto handle = std::move(entry->second);
			handles.erase(entry);
		}
	}
	static int CloseCallback(AVFormatContext *format, AVIOContext *context) noexcept {
		auto &owner = *static_cast<State *>(format->opaque);
		owner.Close(context);
		return owner.error ? AVERROR(EIO) : 0;
	}

	FileSystem &fs;
	string path;
	bool writable;
	optional_ptr<atomic<bool>> cancelled;
	optional_ptr<ClientContext> context;
	std::exception_ptr error;
	unordered_map<AVIOContext *, unique_ptr<Handle>> handles;
};

LerobotVideoIO::LerobotVideoIO(FileSystem &fs, string path, bool writable, optional_ptr<atomic<bool>> cancelled,
                               optional_ptr<ClientContext> context)
    : state(make_uniq<State>(fs, std::move(path), writable, cancelled, context)) {
}

LerobotVideoIO::~LerobotVideoIO() {
}

const char *LerobotVideoIO::URL() {
	return "lerobot-media.mp4";
}

void LerobotVideoIO::Open(AVFormatContext &format) {
	format.opaque = state.get();
	format.io_open = State::OpenCallback;
	format.io_close2 = State::CloseCallback;
	format.flags |= AVFMT_FLAG_CUSTOM_IO;
	format.pb = state->OpenHandle(state->writable);
}

void LerobotVideoIO::Close(AVIOContext *&context) noexcept {
	state->Close(context);
	context = nullptr;
}

void LerobotVideoIO::ThrowIfError() const {
	state->Check();
}

} // namespace duckdb

#endif
