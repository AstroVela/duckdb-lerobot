#include "function/lerobot_video_writer.hpp"
#include "function/lerobot_video_io.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

#include <cmath>
#include <cstring>

#ifdef LEROBOT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif

namespace duckdb {

namespace {

#ifdef LEROBOT_HAVE_FFMPEG

string FFmpegWriteError(int error_code) {
	char buffer[AV_ERROR_MAX_STRING_SIZE];
	if (av_strerror(error_code, buffer, sizeof(buffer)) < 0) {
		return "unknown FFmpeg error " + std::to_string(error_code);
	}
	return buffer;
}

void ThrowOnFFmpegError(int status, const string &operation, const string &path,
                        optional_ptr<LerobotVideoIO> io = nullptr) {
	if (io) {
		io->ThrowIfError();
	}
	if (status < 0) {
		throw IOException("FFmpeg failed to %s LeRobot visual '%s': %s", operation, path, FFmpegWriteError(status));
	}
}

void ThrowOnUnusedOptions(AVDictionary *options, const string &operation, const string &path) {
	if (!options) {
		return;
	}
	string names;
	const AVDictionaryEntry *entry = nullptr;
	while ((entry = av_dict_get(options, "", entry, AV_DICT_IGNORE_SUFFIX))) {
		if (!names.empty()) {
			names += ", ";
		}
		names += entry->key;
	}
	throw InvalidInputException("FFmpeg did not consume LeRobot %s option(s) for '%s': %s", operation, path, names);
}

struct AVPacketDeleter {
	void operator()(AVPacket *packet) const {
		av_packet_free(&packet);
	}
};

struct AVFrameDeleter {
	void operator()(AVFrame *frame) const {
		av_frame_free(&frame);
	}
};

mutex &SVTAV1LifecycleLock() {
	// Older SVT-AV1 releases mutate process-global initialization state from
	// both open and close. Serialize those short lifecycle calls while leaving
	// the substantially longer encode phase concurrent.
	static mutex lifecycle_lock;
	return lifecycle_lock;
}

struct AVCodecContextDeleter {
	explicit AVCodecContextDeleter(bool serialize_svt_p = false) : serialize_svt(serialize_svt_p) {
	}

	void operator()(AVCodecContext *context) const {
		if (serialize_svt) {
			lock_guard<mutex> guard(SVTAV1LifecycleLock());
			avcodec_free_context(&context);
		} else {
			avcodec_free_context(&context);
		}
	}

	bool serialize_svt;
};

struct SwsContextDeleter {
	void operator()(SwsContext *context) const {
		sws_freeContext(context);
	}
};

struct AVFormatInputDeleter {
	explicit AVFormatInputDeleter(LerobotVideoIO &io_p) : io(io_p) {
	}
	void operator()(AVFormatContext *context) const {
		if (!context) {
			return;
		}
		auto pb = context->pb;
		avformat_close_input(&context);
		io.Close(pb);
	}
	LerobotVideoIO &io;
};

struct AVFormatOutputDeleter {
	explicit AVFormatOutputDeleter(LerobotVideoIO &io_p) : io(io_p) {
	}
	void operator()(AVFormatContext *context) const {
		if (!context) {
			return;
		}
		io.Close(context->pb);
		avformat_free_context(context);
	}
	LerobotVideoIO &io;
};

typedef unique_ptr<AVPacket, AVPacketDeleter> PacketPtr;
typedef unique_ptr<AVFrame, AVFrameDeleter> FramePtr;
typedef unique_ptr<AVCodecContext, AVCodecContextDeleter> CodecContextPtr;
typedef unique_ptr<SwsContext, SwsContextDeleter> SwsContextPtr;
typedef unique_ptr<AVFormatContext, AVFormatInputDeleter> FormatInputPtr;
typedef unique_ptr<AVFormatContext, AVFormatOutputDeleter> FormatOutputPtr;

AVPixelFormat SourcePixelFormat(LerobotRawVisualType raw_type) {
	switch (raw_type) {
	case LerobotRawVisualType::RGB24:
		return AV_PIX_FMT_RGB24;
	case LerobotRawVisualType::DEPTH_UINT16:
		return AV_PIX_FMT_GRAY16LE;
	case LerobotRawVisualType::DEPTH_FLOAT32:
		break;
	}
	throw InternalException("Float depth frames must be quantized before selecting an FFmpeg source pixel format");
}

float LoadFloat32(const char *source) {
	float result;
	memcpy(&result, source, sizeof(result));
	return result;
}

uint16_t LoadUInt16(const char *source) {
	uint16_t result;
	memcpy(&result, source, sizeof(result));
	return result;
}

uint16_t QuantizeDepth(float depth, bool millimeters, const LerobotVideoEncodingConfig &options) {
	static const float QMAX = 4095.0f;
	const auto unit_scale = millimeters ? 1000.0 : 1.0;
	const auto depth_min = static_cast<float>(options.depth_min * unit_scale);
	const auto depth_max = static_cast<float>(options.depth_max * unit_scale);
	const auto shift = static_cast<float>(options.depth_shift * unit_scale);
	if (!std::isfinite(depth)) {
		throw InvalidInputException("LeRobot depth frames require finite values");
	}
	if (!options.depth_clip && (depth < depth_min || depth > depth_max)) {
		throw InvalidInputException("LeRobot depth value is outside DEPTH_MIN/DEPTH_MAX with DEPTH_CLIP=false");
	}
	if (options.depth_use_log && !(depth + shift > 0)) {
		throw InvalidInputException("LeRobot logarithmic depth values require depth + DEPTH_SHIFT > 0");
	}
	// The native implementation intentionally performs its ndarray math in
	// float32. Python's scalar log bounds are cast back to the array dtype by
	// NumPy 2.x's weak-scalar promotion rules.
	float normalized;
	if (options.depth_use_log) {
		const auto log_min_double = std::log(static_cast<double>(depth_min + shift));
		const auto log_max_double = std::log(static_cast<double>(depth_max + shift));
		const auto log_value = static_cast<float>(std::log(static_cast<double>(depth + shift)));
		const auto log_min = static_cast<float>(log_min_double);
		const auto log_range = static_cast<float>(log_max_double - log_min_double);
		normalized = static_cast<float>((log_value - log_min) / log_range);
	} else {
		normalized = (depth - depth_min) / (depth_max - depth_min);
	}
	const auto clipped = MaxValue(0.0f, MinValue(1.0f, normalized));
	return static_cast<uint16_t>(std::nearbyint(clipped * QMAX));
}

void FillDepthFrame(const string &raw, LerobotRawVisualType raw_type, idx_t width, idx_t height, AVFrame &frame,
                    const LerobotVideoEncodingConfig &options) {
	for (idx_t y = 0; y < height; y++) {
		auto target = reinterpret_cast<uint16_t *>(frame.data[0] + y * frame.linesize[0]);
		for (idx_t x = 0; x < width; x++) {
			const auto index = y * width + x;
			float value;
			bool millimeters;
			if (raw_type == LerobotRawVisualType::DEPTH_UINT16) {
				value = static_cast<float>(LoadUInt16(raw.data() + index * sizeof(uint16_t)));
				millimeters = true;
			} else {
				value = LoadFloat32(raw.data() + index * sizeof(float));
				millimeters = false;
			}
			target[x] = QuantizeDepth(value, millimeters, options);
		}
	}
}

void DrainEncoder(AVCodecContext &codec_context, AVFormatContext &format_context, AVStream &stream, AVPacket &packet,
                  const string &path, LerobotVideoIO &io) {
	while (true) {
		auto status = avcodec_receive_packet(&codec_context, &packet);
		if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
			return;
		}
		ThrowOnFFmpegError(status, "receive an encoded packet for", path);
		if (packet.duration <= 0) {
			packet.duration = 1;
		}
		av_packet_rescale_ts(&packet, codec_context.time_base, stream.time_base);
		packet.stream_index = stream.index;
		ThrowOnFFmpegError(av_interleaved_write_frame(&format_context, &packet), "mux", path, &io);
		av_packet_unref(&packet);
	}
}

void AppendUInt16(string &result, uint16_t value) {
	result.push_back(static_cast<char>(value & 0xff));
	result.push_back(static_cast<char>((value >> 8) & 0xff));
}

void AppendUInt32(string &result, uint32_t value) {
	for (idx_t byte = 0; byte < 4; byte++) {
		result.push_back(static_cast<char>((value >> (byte * 8)) & 0xff));
	}
}

void AppendTiffEntry(string &result, uint16_t tag, uint16_t type, uint32_t count, uint32_t value) {
	AppendUInt16(result, tag);
	AppendUInt16(result, type);
	AppendUInt32(result, count);
	AppendUInt32(result, value);
}

string EncodeDepthTiff(const string &raw_frame, idx_t width, idx_t height, LerobotRawVisualType raw_type) {
	static const uint16_t ENTRY_COUNT = 11;
	static const uint32_t PIXEL_OFFSET = 8 + 2 + ENTRY_COUNT * 12 + 4;
	const auto bytes_per_sample = raw_type == LerobotRawVisualType::DEPTH_UINT16 ? 2U : 4U;
	const auto byte_count = width * height * bytes_per_sample;
	if (width > NumericLimits<uint32_t>::Maximum() || height > NumericLimits<uint32_t>::Maximum() ||
	    byte_count > NumericLimits<uint32_t>::Maximum()) {
		throw InvalidInputException("LeRobot depth image is too large for TIFF");
	}
	string result;
	result.reserve(PIXEL_OFFSET + byte_count);
	result += "II";
	AppendUInt16(result, 42);
	AppendUInt32(result, 8);
	AppendUInt16(result, ENTRY_COUNT);
	AppendTiffEntry(result, 256, 4, 1, static_cast<uint32_t>(width));
	AppendTiffEntry(result, 257, 4, 1, static_cast<uint32_t>(height));
	AppendTiffEntry(result, 258, 3, 1, bytes_per_sample * 8);
	AppendTiffEntry(result, 259, 3, 1, 1);
	AppendTiffEntry(result, 262, 3, 1, 1);
	AppendTiffEntry(result, 273, 4, 1, PIXEL_OFFSET);
	AppendTiffEntry(result, 277, 3, 1, 1);
	AppendTiffEntry(result, 278, 4, 1, static_cast<uint32_t>(height));
	AppendTiffEntry(result, 279, 4, 1, static_cast<uint32_t>(byte_count));
	AppendTiffEntry(result, 284, 3, 1, 1);
	AppendTiffEntry(result, 339, 3, 1, raw_type == LerobotRawVisualType::DEPTH_UINT16 ? 1 : 3);
	AppendUInt32(result, 0);
	result.append(raw_frame);
	return result;
}

#endif

} // namespace

bool LerobotVisualWriter::Available() {
#ifdef LEROBOT_HAVE_FFMPEG
	return true;
#else
	return false;
#endif
}

idx_t LerobotVisualWriter::ExpectedFrameBytes(idx_t width, idx_t height, LerobotRawVisualType raw_type) {
	if (width == 0 || height == 0 || width > NumericLimits<idx_t>::Maximum() / height) {
		throw InvalidInputException("LeRobot visual dimensions are empty or too large");
	}
	auto bytes = width * height;
	idx_t multiplier;
	switch (raw_type) {
	case LerobotRawVisualType::RGB24:
		multiplier = 3;
		break;
	case LerobotRawVisualType::DEPTH_UINT16:
		multiplier = sizeof(uint16_t);
		break;
	case LerobotRawVisualType::DEPTH_FLOAT32:
		multiplier = sizeof(float);
		break;
	default:
		throw InternalException("Unknown LeRobot raw visual type");
	}
	if (bytes > NumericLimits<idx_t>::Maximum() / multiplier) {
		throw InvalidInputException("LeRobot visual frame byte size is too large");
	}
	return bytes * multiplier;
}

LerobotEncodedVideoInfo LerobotVisualWriter::EncodeVideo(FileSystem &fs, const string &path,
                                                         const string &raw_frames_path, idx_t frame_count,
                                                         const LerobotVideoEncodeOptions &options) {
#ifndef LEROBOT_HAVE_FFMPEG
	throw MissingExtensionException("FORMAT lerobot video writing requires FFmpeg development libraries");
#else
	auto throw_if_cancelled = [&]() {
		if (options.cancelled && options.cancelled->load()) {
			throw InterruptException();
		}
	};
	throw_if_cancelled();
	if (frame_count == 0) {
		throw InvalidInputException("Cannot encode an empty LeRobot video");
	}
	if (options.width > NumericLimits<int>::Maximum() || options.height > NumericLimits<int>::Maximum() ||
	    options.fps > NumericLimits<int>::Maximum()) {
		throw InvalidInputException("LeRobot video dimensions or FPS exceed FFmpeg limits");
	}
	const auto expected_size = ExpectedFrameBytes(options.width, options.height, options.raw_type);
	if (frame_count > NumericLimits<idx_t>::Maximum() / expected_size) {
		throw InvalidInputException("LeRobot raw video spool size is too large");
	}
	const auto expected_file_size = frame_count * expected_size;
	auto raw_frames = fs.OpenFile(raw_frames_path, FileFlags::FILE_FLAGS_READ);
	if (raw_frames->GetFileSize() != expected_file_size) {
		throw IOException("LeRobot raw video spool '%s' has %llu bytes; expected %llu", raw_frames_path,
		                  raw_frames->GetFileSize(), expected_file_size);
	}

	const auto is_depth = options.raw_type != LerobotRawVisualType::RGB24;
	const AVCodec *codec = nullptr;
	const char *required_encoder = "libx265";
	if (is_depth) {
		codec = avcodec_find_encoder_by_name("libx265");
	} else if (!options.encoding.rgb_codec.empty()) {
		// Explicit SQL selections never fall back to another encoder.
		required_encoder = options.encoding.rgb_codec.c_str();
		codec = avcodec_find_encoder_by_name(required_encoder);
	} else {
#if defined(LEROBOT_AV1_ENCODER_SVT)
		required_encoder = "libsvtav1";
		codec = avcodec_find_encoder_by_name(required_encoder);
#elif defined(LEROBOT_AV1_ENCODER_LIBAOM)
		required_encoder = "libaom-av1";
		codec = avcodec_find_encoder_by_name(required_encoder);
#else
		codec = avcodec_find_encoder_by_name("libsvtav1");
		if (!codec) {
			codec = avcodec_find_encoder_by_name("libaom-av1");
		}
		required_encoder = "libsvtav1 or libaom-av1";
#endif
	}
	if (!codec) {
		throw MissingExtensionException("FFmpeg has no %s encoder required by FORMAT lerobot", required_encoder);
	}
	const auto is_svt = !is_depth && strcmp(codec->name, "libsvtav1") == 0;

	LerobotVideoIO io(fs, path, true, options.cancelled);
	AVFormatContext *raw_output = nullptr;
	ThrowOnFFmpegError(avformat_alloc_output_context2(&raw_output, nullptr, "mp4", LerobotVideoIO::URL()),
	                   "allocate the output container for", path);
	if (!raw_output) {
		throw OutOfMemoryException("Failed to allocate a LeRobot MP4 container");
	}
	FormatOutputPtr output(raw_output, AVFormatOutputDeleter(io));
	auto stream = avformat_new_stream(output.get(), nullptr);
	if (!stream) {
		throw OutOfMemoryException("Failed to allocate a LeRobot MP4 stream");
	}
	CodecContextPtr codec_context(avcodec_alloc_context3(codec), AVCodecContextDeleter(is_svt));
	if (!codec_context) {
		throw OutOfMemoryException("Failed to allocate a LeRobot video encoder");
	}
	codec_context->codec_id = codec->id;
	codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
	codec_context->width = NumericCast<int>(options.width);
	codec_context->height = NumericCast<int>(options.height);
	codec_context->pix_fmt = is_depth ? AV_PIX_FMT_GRAY12LE : AV_PIX_FMT_YUV420P;
	codec_context->time_base = AVRational {1, NumericCast<int>(options.fps)};
	codec_context->framerate = AVRational {NumericCast<int>(options.fps), 1};
	// x265 3.5 and 4.1 can corrupt even lossless samples when a reference
	// picture has only one CTU column: its top/bottom right padding is missing.
	// A CTU is at most 64 pixels wide. Intra-only coding avoids that reference
	// path for narrow depth images without changing the process-wide CTU size.
	codec_context->gop_size = is_depth ? (options.width <= 64 ? 1 : 2) : options.encoding.rgb_gop;
	if (options.encoder_threads.IsValid()) {
		codec_context->thread_count = NumericCast<int>(options.encoder_threads.GetIndex());
	}
	if (output->oformat->flags & AVFMT_GLOBALHEADER) {
		codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	AVDictionary *raw_codec_options = nullptr;
	av_dict_set(&raw_codec_options, "g", std::to_string(codec_context->gop_size).c_str(), 0);
	av_dict_set(&raw_codec_options, "crf", std::to_string(is_depth ? 30 : options.encoding.rgb_crf).c_str(), 0);
	if (is_depth) {
		// Each episode is encoded independently then stream-concatenated. Closed
		// GOPs keep keyframe seeks independently decodable at episode boundaries.
		// GOPs of one or two frames need no B-frames. Disable x265's default
		// reorder delay so short and long fragments have consistent DTS offsets.
		codec_context->max_b_frames = 0;
		string x265_parameters = "lossless=1:open-gop=0";
		if (options.encoder_threads.IsValid()) {
			const auto encoder_threads = options.encoder_threads.GetIndex();
			x265_parameters += encoder_threads == 1 ? ":pools=none" : ":pools=" + std::to_string(encoder_threads - 1);
			x265_parameters += ":frame-threads=1";
		}
		av_dict_set(&raw_codec_options, "x265-params", x265_parameters.c_str(), 0);
	} else if (is_svt) {
		av_dict_set(&raw_codec_options, "preset", "12", 0);
		string svt_parameters = "fast-decode=0";
		if (options.encoder_threads.IsValid()) {
			svt_parameters += ":lp=" + std::to_string(options.encoder_threads.GetIndex());
		}
		av_dict_set(&raw_codec_options, "svtav1-params", svt_parameters.c_str(), 0);
	} else {
		av_dict_set(&raw_codec_options, "cpu-used", "8", 0);
		av_dict_set(&raw_codec_options, "b", "0", 0);
	}
	struct DictionaryGuard {
		explicit DictionaryGuard(AVDictionary *&dictionary_p) : dictionary(dictionary_p) {
		}
		~DictionaryGuard() {
			av_dict_free(&dictionary);
		}
		AVDictionary *&dictionary;
	} codec_options_guard(raw_codec_options);
	if (!is_svt) {
		ThrowOnFFmpegError(avcodec_open2(codec_context.get(), codec, &raw_codec_options), "open the encoder for", path);
	} else {
		lock_guard<mutex> guard(SVTAV1LifecycleLock());
		ThrowOnFFmpegError(avcodec_open2(codec_context.get(), codec, &raw_codec_options), "open the encoder for", path);
	}
	ThrowOnUnusedOptions(raw_codec_options, "codec", path);
	ThrowOnFFmpegError(avcodec_parameters_from_context(stream->codecpar, codec_context.get()),
	                   "copy encoder parameters for", path);
	stream->time_base = codec_context->time_base;

	io.Open(*output);
	AVDictionary *raw_format_options = nullptr;
	av_dict_set(&raw_format_options, "movflags", "faststart", 0);
	DictionaryGuard format_options_guard(raw_format_options);
	ThrowOnFFmpegError(avformat_write_header(output.get(), &raw_format_options), "write the MP4 header for", path, &io);
	ThrowOnUnusedOptions(raw_format_options, "container", path);
	throw_if_cancelled();

	FramePtr target_frame(av_frame_alloc());
	PacketPtr packet(av_packet_alloc());
	if (!target_frame || !packet) {
		throw OutOfMemoryException("Failed to allocate LeRobot video frame buffers");
	}
	target_frame->format = codec_context->pix_fmt;
	target_frame->width = codec_context->width;
	target_frame->height = codec_context->height;
	ThrowOnFFmpegError(av_frame_get_buffer(target_frame.get(), 32), "allocate a frame for", path);

	SwsContextPtr scaler;
	if (!is_depth) {
		scaler.reset(sws_getContext(codec_context->width, codec_context->height, SourcePixelFormat(options.raw_type),
		                            codec_context->width, codec_context->height, codec_context->pix_fmt, SWS_BILINEAR,
		                            nullptr, nullptr, nullptr));
		if (!scaler) {
			throw InvalidInputException("FFmpeg cannot convert RGB24 to yuv420p for LeRobot video '%s'", path);
		}
	}

	string raw_frame(expected_size, '\0');
	for (idx_t frame_index = 0; frame_index < frame_count; frame_index++) {
		throw_if_cancelled();
		raw_frames->Read(&raw_frame[0], expected_size, frame_index * expected_size);
		ThrowOnFFmpegError(av_frame_make_writable(target_frame.get()), "make an encoder frame writable for", path);
		if (is_depth) {
			FillDepthFrame(raw_frame, options.raw_type, options.width, options.height, *target_frame, options.encoding);
		} else {
			const uint8_t *source_data[] = {reinterpret_cast<const uint8_t *>(raw_frame.data()), nullptr, nullptr,
			                                nullptr};
			int source_lines[] = {NumericCast<int>(options.width * 3), 0, 0, 0};
			const auto rows = sws_scale(scaler.get(), source_data, source_lines, 0, codec_context->height,
			                            target_frame->data, target_frame->linesize);
			if (rows != codec_context->height) {
				throw IOException("FFmpeg converted only %d of %d rows for LeRobot video '%s'", rows,
				                  codec_context->height, path);
			}
		}
		target_frame->pts = NumericCast<int64_t>(frame_index);
		ThrowOnFFmpegError(avcodec_send_frame(codec_context.get(), target_frame.get()), "submit a frame for", path);
		DrainEncoder(*codec_context, *output, *stream, *packet, path, io);
	}
	throw_if_cancelled();
	ThrowOnFFmpegError(avcodec_send_frame(codec_context.get(), nullptr), "flush the encoder for", path);
	DrainEncoder(*codec_context, *output, *stream, *packet, path, io);
	throw_if_cancelled();
	ThrowOnFFmpegError(av_write_trailer(output.get()), "write the MP4 trailer for", path, &io);
	io.Close(output->pb);
	io.ThrowIfError();

	LerobotEncodedVideoInfo result;
	result.encoder = codec->name;
	result.codec = avcodec_get_name(stream->codecpar->codec_id);
	const auto pixel_format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(stream->codecpar->format));
	if (!pixel_format) {
		throw IOException("LeRobot encoder returned an unknown pixel format");
	}
	result.pixel_format = pixel_format;
	result.gop = codec_context->gop_size;
	result.duration = static_cast<double>(frame_count) / static_cast<double>(options.fps);
	result.frame_count = frame_count;
	return result;
#endif
}

void LerobotVisualWriter::ConcatenateVideos(ClientContext &context, FileSystem &fs, const vector<string> &input_paths,
                                            const string &output_path) {
#ifndef LEROBOT_HAVE_FFMPEG
	throw MissingExtensionException("FORMAT lerobot video writing requires FFmpeg development libraries");
#else
	if (input_paths.empty()) {
		throw InvalidInputException("Cannot concatenate an empty LeRobot video list");
	}
	for (const auto &path : input_paths) {
		if (path == output_path || StringUtil::GetFilePath(path) != StringUtil::GetFilePath(output_path)) {
			throw InvalidInputException("LeRobot concat fragments must be files in the concat staging directory");
		}
	}
	// FFmpeg 6's concat demuxer opens its children without inheriting custom
	// I/O. Remux our single-video fragments directly, so every access uses
	// DuckDB and no filesystem path is interpreted as a URL or concat text.
	LerobotVideoIO output_io(fs, output_path, true, nullptr, &context);
	AVFormatContext *raw_output = nullptr;
	ThrowOnFFmpegError(avformat_alloc_output_context2(&raw_output, nullptr, "mp4", LerobotVideoIO::URL()),
	                   "allocate the concatenated container for", output_path);
	if (!raw_output) {
		throw OutOfMemoryException("Failed to allocate a concatenated LeRobot MP4 container");
	}
	FormatOutputPtr output(raw_output, AVFormatOutputDeleter(output_io));
	AVStream *output_stream = nullptr;
	PacketPtr packet(av_packet_alloc());
	if (!packet) {
		throw OutOfMemoryException("Failed to allocate a concatenated LeRobot packet");
	}
	auto add_timestamp = [&](int64_t left, int64_t right) {
		if ((right > 0 && left > NumericLimits<int64_t>::Maximum() - right) ||
		    (right < 0 && left < NumericLimits<int64_t>::Minimum() - right)) {
			throw IOException("LeRobot video timestamps overflow while concatenating '%s'", output_path);
		}
		return left + right;
	};
	int64_t offset = 0;
	for (const auto &path : input_paths) {
		LerobotVideoIO input_io(fs, path, false, nullptr, &context);
		FormatInputPtr input(avformat_alloc_context(), AVFormatInputDeleter(input_io));
		if (!input) {
			throw OutOfMemoryException("Failed to allocate a LeRobot fragment reader");
		}
		input_io.Open(*input);
		auto raw_input = input.release();
		auto status = avformat_open_input(&raw_input, LerobotVideoIO::URL(), nullptr, nullptr);
		input.reset(raw_input);
		ThrowOnFFmpegError(status, "open a video fragment for", output_path, &input_io);
		ThrowOnFFmpegError(avformat_find_stream_info(input.get(), nullptr), "inspect a video fragment for", output_path,
		                   &input_io);
		if (input->nb_streams != 1 || input->streams[0]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
			throw IOException("LeRobot video fragment '%s' must contain exactly one video stream", path);
		}
		auto source = input->streams[0];
		if (source->duration == AV_NOPTS_VALUE || source->duration <= 0 || source->time_base.num <= 0 ||
		    source->time_base.den <= 0) {
			throw IOException("LeRobot video fragment '%s' has no valid duration or time base", path);
		}
		if (!output_stream) {
			output_stream = avformat_new_stream(output.get(), nullptr);
			if (!output_stream) {
				throw OutOfMemoryException("Failed to allocate a concatenated LeRobot stream");
			}
			ThrowOnFFmpegError(avcodec_parameters_copy(output_stream->codecpar, source->codecpar),
			                   "copy concatenated stream parameters for", output_path);
			output_stream->codecpar->codec_tag = 0;
			output_stream->time_base = source->time_base;
			output_io.Open(*output);
			AVDictionary *options = nullptr;
			av_dict_set(&options, "movflags", "faststart", 0);
			status = avformat_write_header(output.get(), &options);
			av_dict_free(&options);
			ThrowOnFFmpegError(status, "write the MP4 header for", output_path, &output_io);
		} else if (source->codecpar->codec_id != output_stream->codecpar->codec_id ||
		           source->codecpar->format != output_stream->codecpar->format ||
		           source->codecpar->width != output_stream->codecpar->width ||
		           source->codecpar->height != output_stream->codecpar->height) {
			throw IOException("LeRobot video fragment '%s' changes the stream format", path);
		}
		// Accumulate in the output stream's time base. Going through integer
		// microseconds per episode would introduce drift at rates such as 30 fps.
		const auto duration = av_rescale_q(source->duration, source->time_base, output_stream->time_base);
		const auto start = source->start_time == AV_NOPTS_VALUE
		                       ? 0
		                       : av_rescale_q(source->start_time, source->time_base, output_stream->time_base);
		if (duration <= 0 || start == NumericLimits<int64_t>::Minimum()) {
			throw IOException("LeRobot video fragment '%s' has unrepresentable timestamps", path);
		}
		const auto delta = add_timestamp(offset, -start);
		while (true) {
			status = av_read_frame(input.get(), packet.get());
			input_io.ThrowIfError();
			if (status == AVERROR_EOF) {
				break;
			}
			ThrowOnFFmpegError(status, "read a video fragment for", output_path);
			if (packet->stream_index != 0 || packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE) {
				throw IOException("LeRobot video fragment '%s' returned an invalid packet", path);
			}
			av_packet_rescale_ts(packet.get(), source->time_base, output_stream->time_base);
			if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE) {
				throw IOException("LeRobot video fragment '%s' has unrepresentable packet timestamps", path);
			}
			packet->pts = add_timestamp(packet->pts, delta);
			packet->dts = add_timestamp(packet->dts, delta);
			packet->stream_index = output_stream->index;
			packet->pos = -1;
			ThrowOnFFmpegError(av_interleaved_write_frame(output.get(), packet.get()), "mux a concatenated packet for",
			                   output_path, &output_io);
			av_packet_unref(packet.get());
		}
		offset = add_timestamp(offset, duration);
		input.reset();
		input_io.ThrowIfError();
	}
	ThrowOnFFmpegError(av_write_trailer(output.get()), "write the MP4 trailer for", output_path, &output_io);
	output_io.Close(output->pb);
	output_io.ThrowIfError();
#endif
}

struct LerobotImageWriter::Impl {
#ifdef LEROBOT_HAVE_FFMPEG
	Impl(idx_t width, idx_t height) {
		const auto codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
		if (!codec) {
			throw MissingExtensionException("FFmpeg has no PNG encoder required by FORMAT lerobot");
		}
		codec_context.reset(avcodec_alloc_context3(codec));
		if (!codec_context) {
			throw OutOfMemoryException("Failed to allocate the LeRobot PNG encoder");
		}
		codec_context->width = NumericCast<int>(width);
		codec_context->height = NumericCast<int>(height);
		codec_context->pix_fmt = AV_PIX_FMT_RGB24;
		codec_context->time_base = AVRational {1, 1};
		// Each input must immediately produce one independent PNG. Frame
		// threading would introduce delay and retain additional input buffers.
		codec_context->thread_count = 1;
		ThrowOnFFmpegError(avcodec_open2(codec_context.get(), codec, nullptr), "open the PNG encoder for", "memory");
		frame.reset(av_frame_alloc());
		packet.reset(av_packet_alloc());
		if (!frame || !packet) {
			throw OutOfMemoryException("Failed to allocate a LeRobot PNG frame");
		}
		frame->format = codec_context->pix_fmt;
		frame->width = codec_context->width;
		frame->height = codec_context->height;
		frame->pts = 0;
		ThrowOnFFmpegError(av_frame_get_buffer(frame.get(), 1), "allocate a PNG frame for", "memory");
	}

	string Encode(const string &raw_frame, idx_t width, idx_t height) {
		// send_frame may retain a reference even after receive_packet. Never
		// overwrite a referenced buffer when encoding the next still image.
		ThrowOnFFmpegError(av_frame_make_writable(frame.get()), "reuse a PNG frame for", "memory");
		for (idx_t y = 0; y < height; y++) {
			memcpy(frame->data[0] + y * frame->linesize[0], raw_frame.data() + y * width * 3, width * 3);
		}
		ThrowOnFFmpegError(avcodec_send_frame(codec_context.get(), frame.get()), "submit a PNG frame for", "memory");
		ThrowOnFFmpegError(avcodec_receive_packet(codec_context.get(), packet.get()), "encode a PNG frame for",
		                   "memory");
		string result(reinterpret_cast<const char *>(packet->data), packet->size);
		av_packet_unref(packet.get());
		return result;
	}

	CodecContextPtr codec_context;
	FramePtr frame;
	PacketPtr packet;
#endif
};

LerobotImageWriter::LerobotImageWriter(idx_t width_p, idx_t height_p, LerobotRawVisualType raw_type_p)
    : width(width_p), height(height_p), raw_type(raw_type_p),
      expected_size(LerobotVisualWriter::ExpectedFrameBytes(width, height, raw_type)) {
#ifndef LEROBOT_HAVE_FFMPEG
	throw MissingExtensionException("FORMAT lerobot image writing requires FFmpeg development libraries");
#else
	if (raw_type == LerobotRawVisualType::RGB24) {
		impl = make_uniq<Impl>(width, height);
	}
#endif
}

LerobotImageWriter::~LerobotImageWriter() = default;

string LerobotImageWriter::Encode(const string &raw_frame) {
#ifndef LEROBOT_HAVE_FFMPEG
	throw MissingExtensionException("FORMAT lerobot image writing requires FFmpeg development libraries");
#else
	if (raw_frame.size() != expected_size) {
		throw InvalidInputException("LeRobot visual frame has %llu bytes; expected %llu", raw_frame.size(),
		                            expected_size);
	}
	if (raw_type == LerobotRawVisualType::RGB24) {
		return impl->Encode(raw_frame, width, height);
	}
	return EncodeDepthTiff(raw_frame, width, height, raw_type);
#endif
}

} // namespace duckdb
