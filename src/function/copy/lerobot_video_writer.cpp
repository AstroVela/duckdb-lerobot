#include "function/lerobot_video_writer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include <cmath>
#include <cstring>

#ifdef LEROBOT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
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

void ThrowOnFFmpegError(int status, const string &operation, const string &path) {
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

struct AVCodecContextDeleter {
	void operator()(AVCodecContext *context) const {
		avcodec_free_context(&context);
	}
};

struct SwsContextDeleter {
	void operator()(SwsContext *context) const {
		sws_freeContext(context);
	}
};

struct AVFormatInputDeleter {
	void operator()(AVFormatContext *context) const {
		avformat_close_input(&context);
	}
};

struct AVFormatOutputDeleter {
	void operator()(AVFormatContext *context) const {
		if (!context) {
			return;
		}
		if (context->pb) {
			avio_closep(&context->pb);
		}
		avformat_free_context(context);
	}
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

uint16_t QuantizeDepth(float depth, bool millimeters) {
	static const float DEPTH_MIN_METERS = 0.01f;
	static const float DEPTH_MAX_METERS = 10.0f;
	static const float DEPTH_SHIFT_METERS = 3.5f;
	static const float QMAX = 4095.0f;
	const auto unit_scale = millimeters ? 1000.0f : 1.0f;
	const auto depth_min = DEPTH_MIN_METERS * unit_scale;
	const auto depth_max = DEPTH_MAX_METERS * unit_scale;
	const auto shift = DEPTH_SHIFT_METERS * unit_scale;
	const auto log_min_double = std::log(static_cast<double>(depth_min + shift));
	const auto log_max_double = std::log(static_cast<double>(depth_max + shift));
	const auto shifted = depth + shift;
	if (!std::isfinite(depth) || !(shifted > 0)) {
		throw InvalidInputException("LeRobot depth frames require finite values greater than -3.5 metres");
	}
	// The native implementation intentionally performs its ndarray math in
	// float32. Python's scalar log bounds are cast back to the array dtype by
	// NumPy 2.x's weak-scalar promotion rules.
	const auto log_value = static_cast<float>(std::log(static_cast<double>(shifted)));
	const auto log_min = static_cast<float>(log_min_double);
	const auto log_range = static_cast<float>(log_max_double - log_min_double);
	const auto normalized = static_cast<float>((log_value - log_min) / log_range);
	const auto clipped = MaxValue(0.0f, MinValue(1.0f, normalized));
	return static_cast<uint16_t>(std::nearbyint(clipped * QMAX));
}

void FillDepthFrame(const string &raw, LerobotRawVisualType raw_type, idx_t width, idx_t height, AVFrame &frame) {
	const auto pixels = width * height;
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
			target[x] = QuantizeDepth(value, millimeters);
		}
	}
	(void)pixels;
}

void DrainEncoder(AVCodecContext &codec_context, AVFormatContext &format_context, AVStream &stream, AVPacket &packet,
                  const string &path, bool short_depth_video) {
	while (true) {
		auto status = avcodec_receive_packet(&codec_context, &packet);
		if (status == AVERROR(EAGAIN) || status == AVERROR_EOF) {
			return;
		}
		ThrowOnFFmpegError(status, "receive an encoded packet for", path);
		// x265 uses a two-frame DTS reorder window by default. For clips shorter
		// than that window there is no actual reordering, but older x265 releases
		// can return an uninitialized DTS while flushing. Native LeRobot/PyAV
		// produces DTS == PTS for the same one- and two-frame clips.
		if (short_depth_video) {
			packet.dts = packet.pts;
		}
		if (packet.duration <= 0) {
			packet.duration = 1;
		}
		av_packet_rescale_ts(&packet, codec_context.time_base, stream.time_base);
		packet.stream_index = stream.index;
		ThrowOnFFmpegError(av_interleaved_write_frame(&format_context, &packet), "mux", path);
		av_packet_unref(&packet);
	}
}

string EncodeStillImageWithFFmpeg(const string &raw_frame, idx_t width, idx_t height) {
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
	if (!codec) {
		throw MissingExtensionException("FFmpeg has no PNG encoder required by FORMAT lerobot");
	}
	CodecContextPtr codec_context(avcodec_alloc_context3(codec));
	if (!codec_context) {
		throw OutOfMemoryException("Failed to allocate the LeRobot PNG encoder");
	}
	codec_context->width = NumericCast<int>(width);
	codec_context->height = NumericCast<int>(height);
	codec_context->pix_fmt = AV_PIX_FMT_RGB24;
	codec_context->time_base = AVRational {1, 1};
	ThrowOnFFmpegError(avcodec_open2(codec_context.get(), codec, nullptr), "open the PNG encoder for", "memory");

	FramePtr frame(av_frame_alloc());
	PacketPtr packet(av_packet_alloc());
	if (!frame || !packet) {
		throw OutOfMemoryException("Failed to allocate a LeRobot PNG frame");
	}
	frame->format = codec_context->pix_fmt;
	frame->width = codec_context->width;
	frame->height = codec_context->height;
	ThrowOnFFmpegError(av_frame_get_buffer(frame.get(), 1), "allocate a PNG frame for", "memory");
	for (idx_t y = 0; y < height; y++) {
		memcpy(frame->data[0] + y * frame->linesize[0], raw_frame.data() + y * width * 3, width * 3);
	}
	frame->pts = 0;
	ThrowOnFFmpegError(avcodec_send_frame(codec_context.get(), frame.get()), "submit a PNG frame for", "memory");
	ThrowOnFFmpegError(avcodec_receive_packet(codec_context.get(), packet.get()), "encode a PNG frame for", "memory");
	return string(reinterpret_cast<const char *>(packet->data), packet->size);
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
	const AVCodec *codec =
	    is_depth ? avcodec_find_encoder_by_name("libx265") : avcodec_find_encoder_by_name("libsvtav1");
	if (!codec) {
		throw MissingExtensionException("FFmpeg has no %s encoder required by FORMAT lerobot",
		                                is_depth ? "libx265" : "libsvtav1");
	}

	AVFormatContext *raw_output = nullptr;
	ThrowOnFFmpegError(avformat_alloc_output_context2(&raw_output, nullptr, "mp4", path.c_str()),
	                   "allocate the output container for", path);
	if (!raw_output) {
		throw OutOfMemoryException("Failed to allocate a LeRobot MP4 container");
	}
	FormatOutputPtr output(raw_output);
	auto stream = avformat_new_stream(output.get(), nullptr);
	if (!stream) {
		throw OutOfMemoryException("Failed to allocate a LeRobot MP4 stream");
	}
	CodecContextPtr codec_context(avcodec_alloc_context3(codec));
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
	codec_context->gop_size = 2;
	if (output->oformat->flags & AVFMT_GLOBALHEADER) {
		codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	AVDictionary *raw_codec_options = nullptr;
	av_dict_set(&raw_codec_options, "g", "2", 0);
	av_dict_set(&raw_codec_options, "crf", "30", 0);
	if (is_depth) {
		av_dict_set(&raw_codec_options, "x265-params", "lossless=1", 0);
		if (options.encoder_threads.IsValid()) {
			av_dict_set(&raw_codec_options, "threads", std::to_string(options.encoder_threads.GetIndex()).c_str(), 0);
		}
	} else {
		av_dict_set(&raw_codec_options, "preset", "12", 0);
		string svt_parameters = "fast-decode=0";
		if (options.encoder_threads.IsValid()) {
			svt_parameters += ":lp=" + std::to_string(options.encoder_threads.GetIndex());
		}
		av_dict_set(&raw_codec_options, "svtav1-params", svt_parameters.c_str(), 0);
	}
	struct DictionaryGuard {
		explicit DictionaryGuard(AVDictionary *&dictionary_p) : dictionary(dictionary_p) {
		}
		~DictionaryGuard() {
			av_dict_free(&dictionary);
		}
		AVDictionary *&dictionary;
	} codec_options_guard(raw_codec_options);
	ThrowOnFFmpegError(avcodec_open2(codec_context.get(), codec, &raw_codec_options), "open the encoder for", path);
	ThrowOnUnusedOptions(raw_codec_options, "codec", path);
	ThrowOnFFmpegError(avcodec_parameters_from_context(stream->codecpar, codec_context.get()),
	                   "copy encoder parameters for", path);
	stream->time_base = codec_context->time_base;

	if (!(output->oformat->flags & AVFMT_NOFILE)) {
		ThrowOnFFmpegError(avio_open(&output->pb, path.c_str(), AVIO_FLAG_WRITE), "open", path);
	}
	AVDictionary *raw_format_options = nullptr;
	av_dict_set(&raw_format_options, "movflags", "faststart", 0);
	DictionaryGuard format_options_guard(raw_format_options);
	ThrowOnFFmpegError(avformat_write_header(output.get(), &raw_format_options), "write the MP4 header for", path);
	ThrowOnUnusedOptions(raw_format_options, "container", path);

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
		raw_frames->Read(&raw_frame[0], expected_size, frame_index * expected_size);
		ThrowOnFFmpegError(av_frame_make_writable(target_frame.get()), "make an encoder frame writable for", path);
		if (is_depth) {
			FillDepthFrame(raw_frame, options.raw_type, options.width, options.height, *target_frame);
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
		DrainEncoder(*codec_context, *output, *stream, *packet, path, is_depth && frame_count <= 2);
	}
	ThrowOnFFmpegError(avcodec_send_frame(codec_context.get(), nullptr), "flush the encoder for", path);
	DrainEncoder(*codec_context, *output, *stream, *packet, path, is_depth && frame_count <= 2);
	ThrowOnFFmpegError(av_write_trailer(output.get()), "write the MP4 trailer for", path);

	LerobotEncodedVideoInfo result;
	result.codec = is_depth ? "hevc" : "av1";
	result.pixel_format = is_depth ? "gray12le" : "yuv420p";
	result.duration = static_cast<double>(frame_count) / static_cast<double>(options.fps);
	result.frame_count = frame_count;
	return result;
#endif
}

void LerobotVisualWriter::ConcatenateVideos(const vector<string> &input_paths, const string &list_path,
                                            const string &output_path) {
#ifndef LEROBOT_HAVE_FFMPEG
	throw MissingExtensionException("FORMAT lerobot video writing requires FFmpeg development libraries");
#else
	if (input_paths.empty()) {
		throw InvalidInputException("Cannot concatenate an empty LeRobot video list");
	}
	AVDictionary *raw_input_options = nullptr;
	av_dict_set(&raw_input_options, "safe", "0", 0);
	struct DictionaryGuard {
		explicit DictionaryGuard(AVDictionary *&dictionary_p) : dictionary(dictionary_p) {
		}
		~DictionaryGuard() {
			av_dict_free(&dictionary);
		}
		AVDictionary *&dictionary;
	} input_options_guard(raw_input_options);
	AVFormatContext *raw_input = nullptr;
	auto concat_format = av_find_input_format("concat");
	if (!concat_format) {
		throw MissingExtensionException("FFmpeg concat demuxer is unavailable");
	}
	ThrowOnFFmpegError(avformat_open_input(&raw_input, list_path.c_str(), concat_format, &raw_input_options),
	                   "open the concat list for", output_path);
	FormatInputPtr input(raw_input);
	ThrowOnUnusedOptions(raw_input_options, "concat input", output_path);
	ThrowOnFFmpegError(avformat_find_stream_info(input.get(), nullptr), "inspect concat streams for", output_path);

	AVFormatContext *raw_output = nullptr;
	ThrowOnFFmpegError(avformat_alloc_output_context2(&raw_output, nullptr, "mp4", output_path.c_str()),
	                   "allocate the concatenated container for", output_path);
	if (!raw_output) {
		throw OutOfMemoryException("Failed to allocate a concatenated LeRobot MP4 container");
	}
	FormatOutputPtr output(raw_output);
	vector<int> stream_map(input->nb_streams, -1);
	for (idx_t input_index = 0; input_index < input->nb_streams; input_index++) {
		auto input_stream = input->streams[input_index];
		if (input_stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
		    input_stream->codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
		    input_stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
			continue;
		}
		auto output_stream = avformat_new_stream(output.get(), nullptr);
		if (!output_stream) {
			throw OutOfMemoryException("Failed to allocate a concatenated LeRobot stream");
		}
		ThrowOnFFmpegError(avcodec_parameters_copy(output_stream->codecpar, input_stream->codecpar),
		                   "copy concatenated stream parameters for", output_path);
		output_stream->codecpar->codec_tag = 0;
		output_stream->time_base = input_stream->time_base;
		stream_map[input_index] = NumericCast<int>(output_stream->index);
	}
	if (!(output->oformat->flags & AVFMT_NOFILE)) {
		ThrowOnFFmpegError(avio_open(&output->pb, output_path.c_str(), AVIO_FLAG_WRITE), "open", output_path);
	}
	AVDictionary *raw_output_options = nullptr;
	av_dict_set(&raw_output_options, "movflags", "faststart", 0);
	DictionaryGuard output_options_guard(raw_output_options);
	ThrowOnFFmpegError(avformat_write_header(output.get(), &raw_output_options), "write the MP4 header for",
	                   output_path);
	ThrowOnUnusedOptions(raw_output_options, "concat container", output_path);

	PacketPtr packet(av_packet_alloc());
	if (!packet) {
		throw OutOfMemoryException("Failed to allocate a concatenated LeRobot packet");
	}
	while (true) {
		auto status = av_read_frame(input.get(), packet.get());
		if (status == AVERROR_EOF) {
			break;
		}
		ThrowOnFFmpegError(status, "read a concatenated packet for", output_path);
		const auto input_index = packet->stream_index;
		if (input_index < 0 || static_cast<idx_t>(input_index) >= stream_map.size()) {
			throw IOException("FFmpeg returned an invalid stream index while concatenating LeRobot video '%s'",
			                  output_path);
		}
		if (stream_map[input_index] < 0) {
			av_packet_unref(packet.get());
			continue;
		}
		if (packet->pts == AV_NOPTS_VALUE || packet->dts == AV_NOPTS_VALUE) {
			throw IOException("LeRobot video packet has no PTS or DTS while concatenating '%s'", output_path);
		}
		auto input_stream = input->streams[input_index];
		auto output_stream = output->streams[stream_map[input_index]];
		av_packet_rescale_ts(packet.get(), input_stream->time_base, output_stream->time_base);
		packet->stream_index = output_stream->index;
		packet->pos = -1;
		ThrowOnFFmpegError(av_interleaved_write_frame(output.get(), packet.get()), "mux a concatenated packet for",
		                   output_path);
		av_packet_unref(packet.get());
	}
	ThrowOnFFmpegError(av_write_trailer(output.get()), "write the MP4 trailer for", output_path);
#endif
}

string LerobotVisualWriter::EncodeImage(const string &raw_frame, idx_t width, idx_t height,
                                        LerobotRawVisualType raw_type) {
#ifndef LEROBOT_HAVE_FFMPEG
	throw MissingExtensionException("FORMAT lerobot image writing requires FFmpeg development libraries");
#else
	const auto expected_size = ExpectedFrameBytes(width, height, raw_type);
	if (raw_frame.size() != expected_size) {
		throw InvalidInputException("LeRobot visual frame has %llu bytes; expected %llu", raw_frame.size(),
		                            expected_size);
	}
	if (raw_type == LerobotRawVisualType::RGB24) {
		return EncodeStillImageWithFFmpeg(raw_frame, width, height);
	}
	return EncodeDepthTiff(raw_frame, width, height, raw_type);
#endif
}

} // namespace duckdb
