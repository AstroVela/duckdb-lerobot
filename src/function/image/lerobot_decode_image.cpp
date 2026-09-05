#include "function/lerobot_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/main/client_context.hpp"

#include <cstring>

#ifdef LEROBOT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif

namespace duckdb {
namespace {

// Per-image corruption/allocation guards, not a database-wide memory budget.
static const idx_t MAX_IMAGE_BYTES = 64ULL * 1024ULL * 1024ULL;

#ifdef LEROBOT_HAVE_FFMPEG
Value ImageValue(string pixels, int width, int height, int channels, const char *dtype) {
	return Value::STRUCT({{"image", Value::BLOB_RAW(pixels)},
	                      {"width", Value::INTEGER(width)},
	                      {"height", Value::INTEGER(height)},
	                      {"channels", Value::INTEGER(channels)},
	                      {"dtype", Value(dtype)}});
}

// FFmpeg builds differ in TIFF SampleFormat support. Decode the uncompressed
// uint16/float32 depth contract directly so unsupported float TIFFs can never
// silently become integer pixels. PNG and other TIFF formats use FFmpeg below.
bool DecodeDepthTiff(const string_t &input, Value &result, vector<idx_t> &decoder_entries) {
	const auto data = reinterpret_cast<const uint8_t *>(input.GetDataUnsafe());
	const auto size = input.GetSize();
	const bool little = data[0] == 'I';
	auto read = [&](idx_t offset, idx_t bytes) -> uint32_t {
		if (offset > size || bytes > size - offset) {
			throw InvalidInputException("Truncated LeRobot TIFF image");
		}
		uint32_t value = 0;
		for (idx_t i = 0; i < bytes; i++) {
			value |= static_cast<uint32_t>(data[offset + i]) << (8 * (little ? i : bytes - i - 1));
		}
		return value;
	};
	const idx_t directory = read(4, 4);
	const auto count = read(directory, 2);
	if (directory > size || count > (size - directory - 2) / 12) {
		throw InvalidInputException("Truncated LeRobot TIFF directory");
	}
	unordered_map<uint32_t, vector<uint32_t>> fields;
	for (idx_t i = 0; i < count; i++) {
		const auto entry = directory + 2 + i * 12;
		const auto tag = read(entry, 2);
		if (tag != 338 && tag != 339) {
			decoder_entries.push_back(entry);
		}
		if (tag != 256 && tag != 257 && tag != 258 && tag != 259 && tag != 262 && tag != 273 && tag != 274 &&
		    tag != 277 && tag != 278 && tag != 279 && tag != 284 && tag != 338 && tag != 339) {
			continue;
		}
		const auto type = read(entry + 2, 2);
		const auto elements = read(entry + 4, 4);
		const idx_t element_size = type == 3 ? 2 : (type == 4 ? 4 : 0);
		const auto max_elements =
		    tag == 273 || tag == 279 ? size / 4 : (tag == 258 || tag == 338 || tag == 339 ? 4U : 1U);
		if (!element_size || !elements || elements > max_elements || elements > size / element_size ||
		    fields.count(tag)) {
			throw InvalidInputException("Invalid LeRobot TIFF field");
		}
		const idx_t bytes = elements * element_size;
		const idx_t offset = bytes <= 4 ? entry + 8 : read(entry + 8, 4);
		if (offset > size || bytes > size - offset) {
			throw InvalidInputException("Truncated LeRobot TIFF field");
		}
		auto &values = fields[tag];
		for (idx_t j = 0; j < elements; j++) {
			values.push_back(read(offset + j * element_size, element_size));
		}
	}
	auto scalar = [&](uint32_t tag, uint32_t fallback) {
		auto entry = fields.find(tag);
		return entry == fields.end() ? fallback : (entry->second.size() == 1 ? entry->second[0] : 0);
	};
	const auto samples = scalar(277, 1);
	auto uniform = [&](uint32_t tag, uint32_t fallback) {
		auto entry = fields.find(tag);
		if (entry == fields.end()) {
			return fallback;
		}
		const auto &values = entry->second;
		if (values.size() != 1 && values.size() != samples) {
			throw InvalidInputException("Invalid LeRobot TIFF per-channel field count");
		}
		for (auto value : values) {
			if (value != values[0]) {
				throw InvalidInputException("LeRobot TIFF requires uniform sample widths and formats");
			}
		}
		return values[0];
	};
	const auto bits = uniform(258, 1);
	const auto format = uniform(339, 1);
	// Older FFmpeg TIFF decoders ignore SampleFormat. Validate it before the
	// fallback so signed/floating samples cannot silently become unsigned pixels.
	if (!samples || samples > 4 || !bits || (format == 1 ? bits > 16 : !(format == 3 && bits == 32 && samples == 1))) {
		throw InvalidInputException("Unsupported LeRobot TIFF sample width or format");
	}
	auto extra_samples = fields.find(338);
	if (extra_samples != fields.end() &&
	    (samples != 4 || scalar(262, 1) != 2 || extra_samples->second.size() != 1 || extra_samples->second[0] != 2)) {
		throw InvalidInputException("LeRobot TIFF supports only unassociated RGBA extra samples");
	}
	auto &offsets = fields[273];
	auto &counts = fields[279];
	if (offsets.size() != counts.size()) {
		throw InvalidInputException("Invalid LeRobot TIFF strip layout");
	}
	for (idx_t i = 0; i < offsets.size(); i++) {
		if (offsets[i] > size || counts[i] > size - offsets[i]) {
			throw InvalidInputException("Truncated LeRobot TIFF strip");
		}
	}
	if (samples != 1 || !((bits == 16 && format == 1) || (bits == 32 && format == 3))) {
		return false;
	}
	if (scalar(259, 1) != 1) {
		if (format == 3) {
			throw InvalidInputException("LeRobot float32 TIFF requires uncompressed strips");
		}
		return false;
	}
	if (scalar(262, 1) != 1 || scalar(274, 1) != 1 || scalar(284, 1) != 1) {
		throw InvalidInputException("Unsupported LeRobot depth TIFF photometric, orientation or planar layout");
	}
	const idx_t width = scalar(256, 0), height = scalar(257, 0), bytes = bits / 8;
	if (!width || !height || width > MAX_IMAGE_BYTES / bytes || height > MAX_IMAGE_BYTES / (width * bytes)) {
		throw InvalidInputException("LeRobot decoded image exceeds the 64 MiB per-image limit or has zero dimensions");
	}
	const idx_t rows_per_strip = scalar(278, static_cast<uint32_t>(height));
	if (!rows_per_strip || offsets.size() != (height - 1) / rows_per_strip + 1 || counts.size() != offsets.size()) {
		throw InvalidInputException("Invalid LeRobot TIFF strip layout");
	}
	string pixels(width * height * bytes, '\0');
	idx_t position = 0;
	for (idx_t i = 0; i < offsets.size(); i++) {
		const auto rows = MinValue(rows_per_strip, height - i * rows_per_strip);
		const auto length = rows * width * bytes;
		if (counts[i] != length || offsets[i] > size || length > size - offsets[i]) {
			throw InvalidInputException("Truncated or inconsistent LeRobot TIFF strip");
		}
		for (idx_t j = 0; j < length; j += bytes) {
			for (idx_t b = 0; b < bytes; b++) {
				pixels[position + j + b] = data[offsets[i] + j + (little ? b : bytes - b - 1)];
			}
		}
		position += length;
	}
	result = ImageValue(std::move(pixels), static_cast<int>(width), static_cast<int>(height), 1,
	                    format == 3 ? "float32" : "uint16");
	return true;
}

struct ImageDecoder {
	~ImageDecoder() {
		sws_freeContext(scaler);
		av_frame_free(&frame);
		av_packet_free(&packet);
		avcodec_free_context(&codec);
	}
	AVCodecContext *codec = nullptr;
	AVPacket *packet = nullptr;
	AVFrame *frame = nullptr;
	SwsContext *scaler = nullptr;
};

void CheckImageStatus(int status) {
	if (status >= 0) {
		return;
	}
	if (status == AVERROR(ENOMEM)) {
		throw OutOfMemoryException("Failed to allocate LeRobot image decoder buffers");
	}
	char message[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(status, message, sizeof(message));
	throw InvalidInputException("Cannot decode LeRobot PNG/TIFF image: %s", message);
}

int AllocateImageFrame(AVCodecContext *context, AVFrame *frame, int flags) {
	const auto size =
	    av_image_get_buffer_size(static_cast<AVPixelFormat>(frame->format), frame->width, frame->height, 64);
	if (size < 0 || static_cast<idx_t>(size) > MAX_IMAGE_BYTES) {
		return AVERROR(EINVAL);
	}
	return avcodec_default_get_buffer2(context, frame, flags);
}

Value DecodeImage(const string_t &input) {
	const auto size = input.GetSize();
	const auto data = input.GetDataUnsafe();
	if (size > MAX_IMAGE_BYTES) {
		throw InvalidInputException("LeRobot encoded image exceeds the 64 MiB per-image limit");
	}
	AVCodecID codec_id;
	vector<idx_t> tiff_decoder_entries;
	if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
		codec_id = AV_CODEC_ID_PNG;
	} else if (size >= 4 && (memcmp(data, "II\x2a\x00", 4) == 0 || memcmp(data, "MM\x00\x2a", 4) == 0)) {
		codec_id = AV_CODEC_ID_TIFF;
		Value decoded;
		if (DecodeDepthTiff(input, decoded, tiff_decoder_entries)) {
			return decoded;
		}
	} else {
		throw InvalidInputException("lerobot_decode_image requires a PNG or TIFF BLOB");
	}
	const auto decoder = avcodec_find_decoder(codec_id);
	if (!decoder) {
		throw MissingExtensionException("This FFmpeg build has no %s decoder", avcodec_get_name(codec_id));
	}
	ImageDecoder state;
	state.codec = avcodec_alloc_context3(decoder);
	state.packet = av_packet_alloc();
	state.frame = av_frame_alloc();
	if (!state.codec || !state.packet || !state.frame) {
		throw OutOfMemoryException("Failed to allocate LeRobot image decoder");
	}
	state.codec->thread_count = 1;
	state.codec->max_pixels = MAX_IMAGE_BYTES;
	state.codec->get_buffer2 = AllocateImageFrame;
	state.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_EXPLODE;
	CheckImageStatus(avcodec_open2(state.codec, decoder, nullptr));
	const idx_t tiff_directory = (static_cast<idx_t>(size) + 1) & ~idx_t(1);
	const auto packet_size = codec_id == AV_CODEC_ID_TIFF ? tiff_directory + 2 + tiff_decoder_entries.size() * 12 + 4
	                                                      : static_cast<idx_t>(size);
	CheckImageStatus(av_new_packet(state.packet, static_cast<int>(packet_size)));
	memcpy(state.packet->data, data, size); // av_new_packet supplies FFmpeg's required zero padding.
	if (codec_id == AV_CODEC_ID_TIFF) {
		// Keep strict payload decoding: older FFmpeg can otherwise return blank
		// pixels after a decompression error. It rejects SampleFormat/ExtraSamples
		// tags under EXPLODE even for the unsigned, unassociated RGBA samples
		// validated above. Omit only those tags from a new first-page directory.
		// Append it so original pixel data and out-of-line field offsets stay intact.
		memset(state.packet->data + size, 0, packet_size - size);
		auto write = [&](idx_t offset, uint32_t value, idx_t bytes) {
			for (idx_t i = 0; i < bytes; i++) {
				state.packet->data[offset + i] =
				    static_cast<uint8_t>(value >> (8 * (data[0] == 'I' ? i : bytes - i - 1)));
			}
		};
		write(4, static_cast<uint32_t>(tiff_directory), 4);
		write(tiff_directory, static_cast<uint32_t>(tiff_decoder_entries.size()), 2);
		for (idx_t i = 0; i < tiff_decoder_entries.size(); i++) {
			memcpy(state.packet->data + tiff_directory + 2 + i * 12, data + tiff_decoder_entries[i], 12);
		}
	}
	CheckImageStatus(avcodec_send_packet(state.codec, state.packet));
	auto status = avcodec_receive_frame(state.codec, state.frame);
	if (status == AVERROR(EAGAIN)) {
		CheckImageStatus(avcodec_send_packet(state.codec, nullptr));
		status = avcodec_receive_frame(state.codec, state.frame);
	}
	CheckImageStatus(status);
	auto &frame = *state.frame;
	if (frame.decode_error_flags || (frame.flags & AV_FRAME_FLAG_CORRUPT)) {
		throw InvalidInputException("Cannot decode LeRobot PNG/TIFF image: corrupt frame");
	}
	const auto source_format = static_cast<AVPixelFormat>(frame.format);
	const auto descriptor = av_pix_fmt_desc_get(source_format);
	if (!descriptor || frame.width <= 0 || frame.height <= 0) {
		throw InvalidInputException("LeRobot image has an invalid pixel format or dimensions");
	}

	AVPixelFormat output_format;
	const char *dtype;
	idx_t channels;
	idx_t sample_bytes;
	if (source_format == AV_PIX_FMT_GRAYF32LE || source_format == AV_PIX_FMT_GRAYF32BE) {
		output_format = AV_PIX_FMT_GRAYF32LE;
		dtype = "float32";
		channels = 1;
		sample_bytes = 4;
	} else {
		const bool rgb =
		    descriptor->nb_components >= 3 || (descriptor->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) != 0;
		const bool alpha = (descriptor->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
		const auto depth = descriptor->comp[0].depth;
		if ((descriptor->flags & AV_PIX_FMT_FLAG_FLOAT) || depth > 16 || (!rgb && alpha)) {
			throw InvalidInputException("Unsupported LeRobot PNG/TIFF pixel format '%s'", descriptor->name);
		}
		channels = rgb ? (alpha ? 4 : 3) : 1;
		sample_bytes = depth > 8 ? 2 : 1;
		dtype = sample_bytes == 2 ? "uint16" : "uint8";
		output_format = rgb ? (alpha ? (sample_bytes == 2 ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA)
		                             : (sample_bytes == 2 ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24))
		                    : (sample_bytes == 2 ? AV_PIX_FMT_GRAY16LE : AV_PIX_FMT_GRAY8);
	}
	const auto row_bytes = static_cast<idx_t>(frame.width) * channels * sample_bytes;
	if (row_bytes > MAX_IMAGE_BYTES || static_cast<idx_t>(frame.height) > MAX_IMAGE_BYTES / row_bytes) {
		throw InvalidInputException("LeRobot decoded image exceeds the 64 MiB per-image limit");
	}
	string pixels(row_bytes * static_cast<idx_t>(frame.height), '\0');
	if (source_format == output_format || source_format == AV_PIX_FMT_GRAYF32BE) {
		if (!frame.data[0] || std::abs(static_cast<int64_t>(frame.linesize[0])) < static_cast<int64_t>(row_bytes)) {
			throw InvalidInputException("LeRobot image has an invalid row stride");
		}
		for (int y = 0; y < frame.height; y++) {
			memcpy(&pixels[y * row_bytes], frame.data[0] + static_cast<int64_t>(y) * frame.linesize[0], row_bytes);
		}
		if (source_format == AV_PIX_FMT_GRAYF32BE) {
			for (idx_t i = 0; i < pixels.size(); i += 4) {
				std::swap(pixels[i], pixels[i + 3]);
				std::swap(pixels[i + 1], pixels[i + 2]);
			}
		}
	} else {
		state.scaler = sws_getContext(frame.width, frame.height, source_format, frame.width, frame.height,
		                              output_format, SWS_POINT, nullptr, nullptr, nullptr);
		if (!state.scaler) {
			throw InvalidInputException("Cannot convert LeRobot image pixel format '%s'", descriptor->name);
		}
		uint8_t *destination[] = {reinterpret_cast<uint8_t *>(&pixels[0]), nullptr, nullptr, nullptr};
		int strides[] = {static_cast<int>(row_bytes), 0, 0, 0};
		if (sws_scale(state.scaler, frame.data, frame.linesize, 0, frame.height, destination, strides) !=
		    frame.height) {
			throw IOException("FFmpeg returned an incomplete LeRobot image");
		}
	}
	return ImageValue(std::move(pixels), frame.width, frame.height, static_cast<int>(channels), dtype);
}
#endif

void DecodeImages(DataChunk &args, ExpressionState &state, Vector &result) {
	UnifiedVectorFormat input;
	args.data[0].ToUnifiedFormat(args.size(), input);
	const auto blobs = UnifiedVectorFormat::GetData<string_t>(input);
	const auto constant = args.AllConstant();
	const auto count = constant ? MinValue<idx_t>(args.size(), 1) : args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	for (idx_t row = 0; row < count; row++) {
		if (state.GetContext().IsInterrupted()) {
			throw InterruptException();
		}
		const auto index = input.sel->get_index(row);
		if (!input.validity.RowIsValid(index)) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
#ifdef LEROBOT_HAVE_FFMPEG
		result.SetValue(row, DecodeImage(blobs[index]));
#else
		throw MissingExtensionException("lerobot_decode_image requires an FFmpeg-enabled LeRobot build");
#endif
	}
	if (constant) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

} // namespace

ScalarFunctionSet LerobotFunctions::GetDecodeImageFunction() {
	auto type = LogicalType::STRUCT({{"image", LogicalType::BLOB},
	                                 {"width", LogicalType::INTEGER},
	                                 {"height", LogicalType::INTEGER},
	                                 {"channels", LogicalType::INTEGER},
	                                 {"dtype", LogicalType::VARCHAR}});
	return ScalarFunctionSet(ScalarFunction("lerobot_decode_image", {LogicalType::BLOB}, type, DecodeImages));
}

} // namespace duckdb
