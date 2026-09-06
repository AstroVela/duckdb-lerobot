#include "catch.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "function/lerobot_video_writer.hpp"

#include <cstring>

#ifdef LEROBOT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
#endif

using namespace duckdb;

#ifdef LEROBOT_HAVE_FFMPEG
namespace {

struct PNGDecoder {
	PNGDecoder() {
		context = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_PNG));
		packet = av_packet_alloc();
		frame = av_frame_alloc();
	}
	~PNGDecoder() {
		av_frame_free(&frame);
		av_packet_free(&packet);
		avcodec_free_context(&context);
	}
	AVCodecContext *context;
	AVPacket *packet;
	AVFrame *frame;
};

// Open a fresh decoder for each PNG, so an image cannot depend on prior frames.
void CheckPNG(const string &encoded, const string &raw, int width, int height) {
	PNGDecoder decoder;
	REQUIRE(decoder.context);
	REQUIRE(decoder.packet);
	REQUIRE(decoder.frame);
	REQUIRE(avcodec_open2(decoder.context, nullptr, nullptr) == 0);
	REQUIRE(av_new_packet(decoder.packet, encoded.size()) == 0);
	memcpy(decoder.packet->data, encoded.data(), encoded.size());
	REQUIRE(avcodec_send_packet(decoder.context, decoder.packet) == 0);
	REQUIRE(avcodec_receive_frame(decoder.context, decoder.frame) == 0);
	REQUIRE(decoder.frame->width == width);
	REQUIRE(decoder.frame->height == height);
	REQUIRE(decoder.frame->format == AV_PIX_FMT_RGB24);
	string actual;
	for (int y = 0; y < height; y++) {
		actual.append(reinterpret_cast<char *>(decoder.frame->data[0] + y * decoder.frame->linesize[0]), width * 3);
	}
	REQUIRE(actual == raw);
}

string Pixels(idx_t width, idx_t height, idx_t index) {
	string raw(width * height * 3, '\0');
	for (idx_t byte = 0; byte < raw.size(); byte++) {
		raw[byte] = static_cast<char>((byte * 13 + index * 71) % 256);
	}
	return raw;
}

} // namespace

TEST_CASE("Image writers return independent PNGs across reused buffers and separate features", "[image_writer]") {
	for (idx_t copy = 0; copy < 8; copy++) {
		LerobotImageWriter front(31, 17, LerobotRawVisualType::RGB24);
		LerobotImageWriter wrist(1, 1, LerobotRawVisualType::RGB24);
		for (idx_t index = 0; index < 16; index++) {
			const auto raw = Pixels(31, 17, copy * 16 + index);
			const auto small = Pixels(1, 1, index + 100);
			auto first = front.Encode(raw);
			auto second = wrist.Encode(small);
			// Keep earlier outputs alive while both encoders overwrite their
			// buffers. Each returned string must retain its own complete image.
			const auto next = Pixels(31, 17, index + 200);
			auto third = front.Encode(next);
			CheckPNG(third, next, 31, 17);
			CheckPNG(second, small, 1, 1);
			CheckPNG(first, raw, 31, 17);
		}
	}
}

TEST_CASE("Invalid image input does not corrupt an already opened PNG encoder", "[image_writer]") {
	REQUIRE_THROWS_AS(LerobotImageWriter(0, 1, LerobotRawVisualType::RGB24), InvalidInputException);
	REQUIRE_THROWS_AS(LerobotImageWriter(NumericLimits<idx_t>::Maximum(), 2, LerobotRawVisualType::RGB24),
	                  InvalidInputException);
	for (idx_t repeat = 0; repeat < 8; repeat++) {
		LerobotImageWriter writer(5, 3, LerobotRawVisualType::RGB24);
		const auto raw = Pixels(5, 3, repeat);
		CheckPNG(writer.Encode(raw), raw, 5, 3);
		REQUIRE_THROWS_WITH(writer.Encode(raw.substr(1)), Catch::Contains("expected 45"));
		REQUIRE_THROWS_WITH(writer.Encode(raw + "x"), Catch::Contains("expected 45"));
		CheckPNG(writer.Encode(raw), raw, 5, 3);
		// Also destroy a populated writer directly during exception unwinding.
		REQUIRE_THROWS_AS(
		    [&]() {
			    LerobotImageWriter abandoned(5, 3, LerobotRawVisualType::RGB24);
			    abandoned.Encode(raw);
			    throw IOException("abandon COPY");
		    }(),
		    IOException);
	}
}
#else
TEST_CASE("Image writing reports missing FFmpeg support", "[image_writer]") {
	REQUIRE_THROWS_AS(LerobotImageWriter(1, 1, LerobotRawVisualType::RGB24), MissingExtensionException);
}
#endif
