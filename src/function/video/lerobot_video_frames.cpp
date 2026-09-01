#include "function/lerobot_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"

#include "function/lerobot_multi_file_reader.hpp"
#include "storage/lerobot_metadata_cache.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <type_traits>
#include <utility>

#ifdef LEROBOT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#endif

namespace duckdb {

namespace {

static const idx_t LEROBOT_DEFAULT_DECODE_BATCH_SIZE = 16;
static const idx_t LEROBOT_DECODE_FRAME_BUDGET = 20000;
static const double LEROBOT_DEFAULT_CLUSTER_GAP_SECONDS = 10.0;

template <typename CALLBACK>
struct BindColumnNames;

template <typename RESULT, typename CONTEXT, typename INPUT, typename RETURN_TYPES, typename COLUMN_NAMES>
struct BindColumnNames<RESULT (*)(CONTEXT, INPUT, RETURN_TYPES, COLUMN_NAMES)> {
	using type = typename std::remove_reference<COLUMN_NAMES>::type;
};

using LerobotColumnNames = typename BindColumnNames<table_function_bind_t>::type;

template <typename CHUNK>
auto SetOutputCardinality(CHUNK &output, idx_t count, int) -> decltype(output.SetCardinalityUnsafe(count), void()) {
	output.SetCardinalityUnsafe(count);
}

template <typename CHUNK>
void SetOutputCardinality(CHUNK &output, idx_t count, long) {
	output.SetCardinality(count);
}

template <typename CONTEXT>
auto CheckForInterrupt(CONTEXT &context, int) -> decltype(context.InterruptCheck(), void()) {
	context.InterruptCheck();
}

template <typename CONTEXT>
void CheckForInterrupt(CONTEXT &, long) {
}

bool GetRefreshParameter(TableFunctionBindInput &input) {
	auto entry = input.named_parameters.find("refresh");
	if (entry == input.named_parameters.end()) {
		return false;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames refresh must not be NULL");
	}
	return BooleanValue::Get(entry->second);
}

vector<int64_t> GetNonNegativeIndices(const Value &value, const char *parameter_name) {
	if (value.IsNull()) {
		throw BinderException("lerobot_video_frames %s must not be NULL", parameter_name);
	}
	vector<int64_t> result;
	for (const auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw BinderException("lerobot_video_frames %s must not contain NULL", parameter_name);
		}
		auto index = child.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		if (index < 0) {
			throw BinderException("LeRobot %s must be non-negative", parameter_name);
		}
		result.push_back(index);
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

vector<string> GetVideoKeys(TableFunctionBindInput &input, const LerobotVideoMetadata &metadata) {
	auto entry = input.named_parameters.find("video_keys");
	if (entry == input.named_parameters.end()) {
		return metadata.GetVideoKeys();
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames video_keys must not be NULL");
	}
	vector<string> result;
	for (const auto &child : ListValue::GetChildren(entry->second)) {
		if (child.IsNull()) {
			throw BinderException("lerobot_video_frames video_keys must not contain NULL");
		}
		auto key = StringValue::Get(child);
		if (key.empty()) {
			throw BinderException("lerobot_video_frames video_keys must not contain an empty key");
		}
		result.push_back(std::move(key));
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}

int64_t GetNamedInteger(TableFunctionBindInput &input, const char *name, int64_t default_value) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end()) {
		return default_value;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames %s must not be NULL", name);
	}
	return entry->second.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
}

double GetNamedDouble(TableFunctionBindInput &input, const char *name, double default_value) {
	auto entry = input.named_parameters.find(name);
	if (entry == input.named_parameters.end()) {
		return default_value;
	}
	if (entry->second.IsNull()) {
		throw BinderException("lerobot_video_frames %s must not be NULL", name);
	}
	return entry->second.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
}

string ValueListSQL(const vector<string> &values) {
	string result = "[";
	for (idx_t index = 0; index < values.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		result += Value(values[index]).ToSQLString();
	}
	result += "]";
	return result;
}

string IntegerListSQL(const vector<int64_t> &values) {
	string result = "(";
	for (idx_t index = 0; index < values.size(); index++) {
		if (index > 0) {
			result += ", ";
		}
		result += std::to_string(values[index]);
	}
	result += ")";
	return result;
}

struct LerobotDecodeTarget {
	LerobotDecodeTarget(int64_t episode_index_p, int64_t frame_index_p, double frame_timestamp_p,
	                    double video_timestamp_p, idx_t route_index_p)
	    : episode_index(episode_index_p), frame_index(frame_index_p), frame_timestamp(frame_timestamp_p),
	      video_timestamp(video_timestamp_p), route_index(route_index_p) {
	}

	int64_t episode_index;
	int64_t frame_index;
	double frame_timestamp;
	double video_timestamp;
	idx_t route_index;
};

struct LerobotDecodeJob {
	string video_path;
	vector<vector<idx_t>> clusters;
};

struct LerobotVideoFramesBindData final : public TableFunctionData {
	LerobotVideoFramesBindData(shared_ptr<LerobotVideoMetadata> metadata_p, vector<LerobotVideoRoute> routes_p,
	                           vector<LerobotDecodeTarget> targets_p, vector<LerobotDecodeJob> jobs_p,
	                           double tolerance_p, int32_t width_p, int32_t height_p, idx_t output_batch_size_p)
	    : metadata(std::move(metadata_p)), routes(std::move(routes_p)), targets(std::move(targets_p)),
	      jobs(std::move(jobs_p)), tolerance(tolerance_p), width(width_p), height(height_p),
	      output_batch_size(output_batch_size_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LerobotVideoFramesBindData>(metadata, routes, targets, jobs, tolerance, width, height,
		                                             output_batch_size);
	}

	shared_ptr<LerobotVideoMetadata> metadata;
	vector<LerobotVideoRoute> routes;
	vector<LerobotDecodeTarget> targets;
	vector<LerobotDecodeJob> jobs;
	double tolerance;
	int32_t width;
	int32_t height;
	idx_t output_batch_size;
};

vector<LerobotDecodeJob> BuildDecodeJobs(const LerobotVideoFramesBindData &bind_data, double cluster_gap) {
	vector<idx_t> order;
	order.reserve(bind_data.targets.size());
	for (idx_t index = 0; index < bind_data.targets.size(); index++) {
		order.push_back(index);
	}
	std::sort(order.begin(), order.end(), [&bind_data](idx_t left_index, idx_t right_index) {
		const auto &left = bind_data.targets[left_index];
		const auto &right = bind_data.targets[right_index];
		const auto &left_path = bind_data.metadata->GetVideoFile(bind_data.routes[left.route_index]);
		const auto &right_path = bind_data.metadata->GetVideoFile(bind_data.routes[right.route_index]);
		if (left_path != right_path) {
			return left_path < right_path;
		}
		if (left.video_timestamp != right.video_timestamp) {
			return left.video_timestamp < right.video_timestamp;
		}
		if (left.episode_index != right.episode_index) {
			return left.episode_index < right.episode_index;
		}
		return left.frame_index < right.frame_index;
	});

	vector<LerobotDecodeJob> jobs;
	for (const auto target_index : order) {
		const auto &target = bind_data.targets[target_index];
		const auto &path = bind_data.metadata->GetVideoFile(bind_data.routes[target.route_index]);
		if (jobs.empty() || jobs.back().video_path != path) {
			jobs.push_back(LerobotDecodeJob());
			jobs.back().video_path = path;
		}
		auto &job = jobs.back();
		bool new_cluster = job.clusters.empty();
		if (!new_cluster) {
			const auto previous_index = job.clusters.back().back();
			const auto gap = target.video_timestamp - bind_data.targets[previous_index].video_timestamp;
			new_cluster = gap > cluster_gap;
		}
		if (new_cluster) {
			job.clusters.push_back(vector<idx_t>());
		}
		job.clusters.back().push_back(target_index);
	}
	return jobs;
}

unique_ptr<FunctionData> LerobotVideoFramesBind(ClientContext &context, TableFunctionBindInput &input,
                                                vector<LogicalType> &return_types, LerobotColumnNames &names) {
	if (input.inputs[0].IsNull()) {
		throw BinderException("lerobot_video_frames root must not be NULL");
	}
	auto root = NormalizeLerobotRoot(StringValue::Get(input.inputs[0]));
	auto episode_indices = GetNonNegativeIndices(input.inputs[1], "episode_indices");
	const auto refresh = GetRefreshParameter(input);

	bool cache_hit;
	auto video_metadata = LerobotVideoMetadata::Get(context, root, refresh, cache_hit);
	auto video_keys = GetVideoKeys(input, *video_metadata);
	auto routes = video_metadata->ResolveRoutes(episode_indices, video_keys);

	vector<int64_t> frame_indices;
	auto frame_filter = input.named_parameters.find("frame_indices");
	if (frame_filter != input.named_parameters.end()) {
		frame_indices = GetNonNegativeIndices(frame_filter->second, "frame_indices");
	}

	const auto width_value = GetNamedInteger(input, "width", 0);
	const auto height_value = GetNamedInteger(input, "height", 0);
	if ((width_value == 0) != (height_value == 0)) {
		throw BinderException("lerobot_video_frames width and height must either both be zero or both be positive");
	}
	if (width_value < 0 || height_value < 0 || width_value > 32768 || height_value > 32768) {
		throw BinderException("lerobot_video_frames width and height must be between 0 and 32768");
	}

	const auto default_tolerance = 0.5 / static_cast<double>(video_metadata->GetFPS());
	const auto tolerance = GetNamedDouble(input, "tolerance", default_tolerance);
	if (!std::isfinite(tolerance) || tolerance <= 0) {
		throw BinderException("lerobot_video_frames tolerance must be finite and positive");
	}
	const auto cluster_gap = GetNamedDouble(input, "cluster_gap", LEROBOT_DEFAULT_CLUSTER_GAP_SECONDS);
	if (!std::isfinite(cluster_gap) || cluster_gap < 0) {
		throw BinderException("lerobot_video_frames cluster_gap must be finite and non-negative");
	}
	const auto batch_size_value = GetNamedInteger(input, "batch_size", LEROBOT_DEFAULT_DECODE_BATCH_SIZE);
	if (batch_size_value <= 0 || batch_size_value > static_cast<int64_t>(STANDARD_VECTOR_SIZE)) {
		throw BinderException("lerobot_video_frames batch_size must be between 1 and %d", STANDARD_VECTOR_SIZE);
	}

	names = {"episode_index",     "frame_index", "timestamp", "video_key", "video_path", "video_timestamp",
	         "decoded_timestamp", "width",       "height",    "channels",  "image"};
	return_types = {LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::DOUBLE, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::DOUBLE,  LogicalType::DOUBLE, LogicalType::INTEGER,
	                LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::BLOB};

	vector<LerobotDecodeTarget> targets;
	if (!episode_indices.empty() && !routes.empty() &&
	    (frame_filter == input.named_parameters.end() || !frame_indices.empty())) {
		bool data_cache_hit;
		// LerobotVideoMetadata::Get above has already refreshed and validated the
		// shared base metadata cache when refresh was requested.
		auto dataset_metadata = LerobotDatasetMetadata::Get(context, root, false, data_cache_hit);
		auto data_files = dataset_metadata->ResolveDataFiles(episode_indices);
		if (!data_files.empty()) {
			string query = "SELECT CAST(episode_index AS BIGINT), CAST(frame_index AS BIGINT), "
			               "CAST(timestamp AS DOUBLE) FROM read_parquet(" +
			               ValueListSQL(data_files) + ") WHERE episode_index IN " + IntegerListSQL(episode_indices);
			if (frame_filter != input.named_parameters.end()) {
				query += " AND frame_index IN " + IntegerListSQL(frame_indices);
			}

			Connection connection(*context.db);
			auto frame_result = connection.Query(query);
			if (frame_result->HasError()) {
				throw BinderException("Failed to read LeRobot frame timestamps: %s", frame_result->GetError());
			}

			unordered_map<int64_t, vector<idx_t>> routes_by_episode;
			for (idx_t route_index = 0; route_index < routes.size(); route_index++) {
				routes_by_episode[routes[route_index].episode_index].push_back(route_index);
			}
			while (true) {
				auto chunk = frame_result->Fetch();
				if (!chunk) {
					break;
				}
				for (idx_t row = 0; row < chunk->size(); row++) {
					for (idx_t column = 0; column < 3; column++) {
						if (chunk->GetValue(column, row).IsNull()) {
							throw BinderException("LeRobot frame alignment columns must not contain NULL");
						}
					}
					const auto episode_index = chunk->GetValue(0, row).GetValue<int64_t>();
					const auto frame_index = chunk->GetValue(1, row).GetValue<int64_t>();
					const auto frame_timestamp = chunk->GetValue(2, row).GetValue<double>();
					if (episode_index < 0 || frame_index < 0 || !std::isfinite(frame_timestamp) ||
					    frame_timestamp < 0) {
						throw BinderException("Invalid LeRobot frame alignment metadata for episode %d, frame %d",
						                      episode_index, frame_index);
					}
					auto route_entry = routes_by_episode.find(episode_index);
					if (route_entry == routes_by_episode.end()) {
						continue;
					}
					for (const auto route_index : route_entry->second) {
						const auto video_timestamp = routes[route_index].from_timestamp + frame_timestamp;
						if (!std::isfinite(video_timestamp)) {
							throw BinderException("Invalid absolute LeRobot video timestamp for episode %d, frame %d",
							                      episode_index, frame_index);
						}
						targets.push_back(LerobotDecodeTarget(episode_index, frame_index, frame_timestamp,
						                                      video_timestamp, route_index));
					}
				}
			}
		}
	}

	vector<LerobotDecodeJob> empty_jobs;
	auto result = make_uniq<LerobotVideoFramesBindData>(
	    std::move(video_metadata), std::move(routes), std::move(targets), std::move(empty_jobs), tolerance,
	    static_cast<int32_t>(width_value), static_cast<int32_t>(height_value), static_cast<idx_t>(batch_size_value));
	result->jobs = BuildDecodeJobs(*result, cluster_gap);
	return result;
}

struct LerobotVideoFramesGlobalState final : public GlobalTableFunctionState {
	explicit LerobotVideoFramesGlobalState(idx_t job_count_p) : job_count(job_count_p) {
	}

	idx_t MaxThreads() const override {
		return job_count == 0 ? 1 : job_count;
	}

	bool ClaimJob(idx_t &job_index) {
		lock_guard<mutex> guard(lock);
		if (next_job >= job_count) {
			return false;
		}
		job_index = next_job++;
		return true;
	}

	mutex lock;
	idx_t next_job = 0;
	idx_t job_count;
};

unique_ptr<GlobalTableFunctionState> LerobotVideoFramesInitGlobal(ClientContext &, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoFramesBindData>();
	return make_uniq<LerobotVideoFramesGlobalState>(bind_data.jobs.size());
}

struct DecodedVideoFrame {
	idx_t target_index;
	double decoded_timestamp;
	int32_t width;
	int32_t height;
	string pixels;
};

#ifdef LEROBOT_HAVE_FFMPEG

string FFmpegError(int error_code) {
	char buffer[AV_ERROR_MAX_STRING_SIZE];
	if (av_strerror(error_code, buffer, sizeof(buffer)) < 0) {
		return "unknown FFmpeg error " + std::to_string(error_code);
	}
	return string(buffer);
}

struct DuckDBAVIOState {
	DuckDBAVIOState(ClientContext &context, const string &path)
	    : handle(FileSystem::GetFileSystem(context).OpenFile(path, FileFlags::FILE_FLAGS_READ)), position(0) {
		auto file_size = handle->GetFileSize();
		if (file_size > static_cast<idx_t>(std::numeric_limits<int64_t>::max())) {
			throw IOException("LeRobot video file is too large for FFmpeg: '%s'", path);
		}
		size = static_cast<int64_t>(file_size);
	}

	static int Read(void *opaque, uint8_t *buffer, int buffer_size) {
		auto &state = *reinterpret_cast<DuckDBAVIOState *>(opaque);
		try {
			auto read_count = state.handle->Read(buffer, static_cast<idx_t>(buffer_size));
			if (read_count <= 0) {
				return AVERROR_EOF;
			}
			state.position += read_count;
			return static_cast<int>(read_count);
		} catch (std::exception &exception) {
			state.error = exception.what();
			return AVERROR(EIO);
		}
	}

	static int64_t Seek(void *opaque, int64_t offset, int whence) {
		auto &state = *reinterpret_cast<DuckDBAVIOState *>(opaque);
		whence &= ~AVSEEK_FORCE;
		if (whence == AVSEEK_SIZE) {
			return state.size;
		}
		int64_t base_position;
		if (whence == SEEK_SET) {
			base_position = 0;
		} else if (whence == SEEK_CUR) {
			base_position = state.position;
		} else if (whence == SEEK_END) {
			base_position = state.size;
		} else {
			return AVERROR(EINVAL);
		}
		if ((offset > 0 && base_position > std::numeric_limits<int64_t>::max() - offset) ||
		    (offset < 0 && base_position < std::numeric_limits<int64_t>::min() - offset)) {
			return AVERROR(EINVAL);
		}
		const auto next_position = base_position + offset;
		if (next_position < 0 || next_position > state.size) {
			return AVERROR(EINVAL);
		}
		try {
			state.handle->Seek(static_cast<idx_t>(next_position));
			state.position = next_position;
			return next_position;
		} catch (std::exception &exception) {
			state.error = exception.what();
			return AVERROR(EIO);
		}
	}

	void ThrowIOError(const string &path) {
		if (!error.empty()) {
			throw IOException("Failed to read LeRobot video '%s': %s", path, error);
		}
	}

	unique_ptr<FileHandle> handle;
	int64_t size;
	int64_t position;
	string error;
};

class LerobotClusterDecoder {
public:
	LerobotClusterDecoder(ClientContext &context_p, const LerobotVideoFramesBindData &bind_data_p,
	                      const LerobotDecodeJob &job_p)
	    : context(context_p), bind_data(bind_data_p), job(job_p), io_state(context_p, job_p.video_path),
	      format_context(nullptr), avio_context(nullptr), codec_context(nullptr), packet(nullptr),
	      previous_frame(nullptr), current_frame(nullptr), video_stream(nullptr), sws_context(nullptr),
	      cluster_position(0), target_position(0), decoded_frames_since_target(0), demux_eof(false), flush_sent(false),
	      decoder_eof(false), have_previous(false), have_current(false), previous_timestamp(0), current_timestamp(0),
	      sws_source_width(0), sws_source_height(0), sws_source_format(AV_PIX_FMT_NONE), resize_source_width(0),
	      resize_source_height(0), resize_target_width(0), resize_target_height(0) {
		try {
			Open();
			StartCluster();
		} catch (...) {
			Close();
			throw;
		}
	}

	~LerobotClusterDecoder() {
		Close();
	}

	void Close() {
		if (sws_context) {
			sws_freeContext(sws_context);
			sws_context = nullptr;
		}
		if (previous_frame) {
			av_frame_free(&previous_frame);
		}
		if (current_frame) {
			av_frame_free(&current_frame);
		}
		if (packet) {
			av_packet_free(&packet);
		}
		if (codec_context) {
			avcodec_free_context(&codec_context);
		}
		if (format_context) {
			avformat_close_input(&format_context);
		}
		if (avio_context) {
			avio_context_free(&avio_context);
		}
	}

	bool Next(DecodedVideoFrame &result) {
		while (cluster_position < job.clusters.size()) {
			auto &cluster = job.clusters[cluster_position];
			if (target_position >= cluster.size()) {
				cluster_position++;
				if (cluster_position >= job.clusters.size()) {
					return false;
				}
				StartCluster();
				continue;
			}
			CheckForInterrupt(context, 0);
			const auto target_index = cluster[target_position];
			const auto target_timestamp = bind_data.targets[target_index].video_timestamp;
			if (have_current && target_timestamp <= current_timestamp) {
				const AVFrame *selected = current_frame;
				double selected_timestamp = current_timestamp;
				if (have_previous && std::fabs(previous_timestamp - target_timestamp) <=
				                         std::fabs(current_timestamp - target_timestamp)) {
					selected = previous_frame;
					selected_timestamp = previous_timestamp;
				}
				ValidateTolerance(target_timestamp, selected_timestamp);
				ConvertFrame(*selected, selected_timestamp, target_index, result);
				target_position++;
				decoded_frames_since_target = 0;
				return true;
			}

			if (have_current) {
				av_frame_unref(previous_frame);
				av_frame_move_ref(previous_frame, current_frame);
				previous_timestamp = current_timestamp;
				have_previous = true;
				have_current = false;
			}

			if (decoder_eof || !ReadFrame()) {
				decoder_eof = true;
				if (!have_previous) {
					throw InvalidInputException("No frames decoded from LeRobot video '%s'", job.video_path);
				}
				ValidateTolerance(target_timestamp, previous_timestamp);
				ConvertFrame(*previous_frame, previous_timestamp, target_index, result);
				target_position++;
				decoded_frames_since_target = 0;
				return true;
			}
			have_current = true;
		}
		return false;
	}

private:
	void Open() {
		const idx_t io_buffer_size = 64 * 1024;
		auto io_buffer = reinterpret_cast<unsigned char *>(av_malloc(io_buffer_size));
		if (!io_buffer) {
			throw OutOfMemoryException("Failed to allocate FFmpeg IO buffer");
		}
		avio_context = avio_alloc_context(io_buffer, static_cast<int>(io_buffer_size), 0, &io_state,
		                                  DuckDBAVIOState::Read, nullptr, DuckDBAVIOState::Seek);
		if (!avio_context) {
			av_free(io_buffer);
			throw OutOfMemoryException("Failed to allocate FFmpeg AVIO context");
		}

		format_context = avformat_alloc_context();
		if (!format_context) {
			throw OutOfMemoryException("Failed to allocate FFmpeg format context");
		}
		format_context->pb = avio_context;
		format_context->flags |= AVFMT_FLAG_CUSTOM_IO;
		auto status = avformat_open_input(&format_context, nullptr, nullptr, nullptr);
		io_state.ThrowIOError(job.video_path);
		if (status < 0) {
			throw IOException("FFmpeg could not open LeRobot video '%s': %s", job.video_path, FFmpegError(status));
		}
		status = avformat_find_stream_info(format_context, nullptr);
		io_state.ThrowIOError(job.video_path);
		if (status < 0) {
			throw IOException("FFmpeg could not inspect LeRobot video '%s': %s", job.video_path, FFmpegError(status));
		}

		const auto stream_index = av_find_best_stream(format_context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		if (stream_index < 0) {
			throw InvalidInputException("LeRobot video '%s' has no decodable video stream", job.video_path);
		}
		video_stream = format_context->streams[stream_index];
		const AVCodec *codec = avcodec_find_decoder(video_stream->codecpar->codec_id);
		if (!codec) {
			throw InvalidInputException("LeRobot video '%s' uses an FFmpeg codec with no available decoder",
			                            job.video_path);
		}
		codec_context = avcodec_alloc_context3(codec);
		if (!codec_context) {
			throw OutOfMemoryException("Failed to allocate FFmpeg codec context");
		}
		status = avcodec_parameters_to_context(codec_context, video_stream->codecpar);
		if (status < 0) {
			throw IOException("FFmpeg could not configure decoder for '%s': %s", job.video_path, FFmpegError(status));
		}
		status = avcodec_open2(codec_context, codec, nullptr);
		if (status < 0) {
			throw IOException("FFmpeg could not start decoder for '%s': %s", job.video_path, FFmpegError(status));
		}

		packet = av_packet_alloc();
		previous_frame = av_frame_alloc();
		current_frame = av_frame_alloc();
		if (!packet || !previous_frame || !current_frame) {
			throw OutOfMemoryException("Failed to allocate FFmpeg decode frames");
		}
	}

	void StartCluster() {
		av_packet_unref(packet);
		av_frame_unref(previous_frame);
		av_frame_unref(current_frame);
		target_position = 0;
		decoded_frames_since_target = 0;
		demux_eof = false;
		flush_sent = false;
		decoder_eof = false;
		have_previous = false;
		have_current = false;

		const auto &cluster = job.clusters[cluster_position];
		const auto earliest = bind_data.targets[cluster.front()].video_timestamp;
		if (earliest > static_cast<double>(std::numeric_limits<int64_t>::max()) / static_cast<double>(AV_TIME_BASE)) {
			throw InvalidInputException("LeRobot video timestamp %.6f is too large to seek in '%s'", earliest,
			                            job.video_path);
		}
		const auto seek_timestamp = static_cast<int64_t>(std::max(0.0, earliest) * static_cast<double>(AV_TIME_BASE));
		auto status = av_seek_frame(format_context, -1, seek_timestamp, AVSEEK_FLAG_BACKWARD);
		io_state.ThrowIOError(job.video_path);
		if (status < 0) {
			throw IOException("FFmpeg could not seek LeRobot video '%s' to %.6f seconds: %s", job.video_path, earliest,
			                  FFmpegError(status));
		}
		avcodec_flush_buffers(codec_context);
	}

	bool ReadFrame() {
		av_frame_unref(current_frame);
		while (true) {
			auto status = avcodec_receive_frame(codec_context, current_frame);
			if (status == 0) {
				int64_t timestamp = current_frame->best_effort_timestamp;
				if (timestamp == AV_NOPTS_VALUE) {
					timestamp = current_frame->pts;
				}
				if (timestamp == AV_NOPTS_VALUE) {
					av_frame_unref(current_frame);
					continue;
				}
				current_timestamp = static_cast<double>(timestamp) * av_q2d(video_stream->time_base);
				if (!std::isfinite(current_timestamp)) {
					av_frame_unref(current_frame);
					continue;
				}
				if (have_previous && current_timestamp + 1e-9 < previous_timestamp) {
					throw InvalidInputException("FFmpeg returned non-monotonic timestamps for LeRobot video '%s'",
					                            job.video_path);
				}
				decoded_frames_since_target++;
				if ((decoded_frames_since_target & 255) == 0) {
					CheckForInterrupt(context, 0);
				}
				if (decoded_frames_since_target > LEROBOT_DECODE_FRAME_BUDGET) {
					throw InvalidInputException("Exceeded the %d-frame decode budget while aligning LeRobot video '%s'",
					                            LEROBOT_DECODE_FRAME_BUDGET, job.video_path);
				}
				return true;
			}
			if (status == AVERROR_EOF) {
				return false;
			}
			if (status != AVERROR(EAGAIN)) {
				throw IOException("FFmpeg failed while decoding LeRobot video '%s': %s", job.video_path,
				                  FFmpegError(status));
			}

			if (demux_eof) {
				if (!flush_sent) {
					status = avcodec_send_packet(codec_context, nullptr);
					flush_sent = true;
					if (status < 0 && status != AVERROR_EOF) {
						throw IOException("FFmpeg failed to flush LeRobot video '%s': %s", job.video_path,
						                  FFmpegError(status));
					}
					continue;
				}
				return false;
			}

			while (true) {
				status = av_read_frame(format_context, packet);
				io_state.ThrowIOError(job.video_path);
				if (status < 0) {
					if (status != AVERROR_EOF) {
						throw IOException("FFmpeg failed while reading LeRobot video '%s': %s", job.video_path,
						                  FFmpegError(status));
					}
					demux_eof = true;
					break;
				}
				if (packet->stream_index != video_stream->index) {
					av_packet_unref(packet);
					continue;
				}
				status = avcodec_send_packet(codec_context, packet);
				av_packet_unref(packet);
				if (status < 0) {
					throw IOException("FFmpeg failed to submit a packet for LeRobot video '%s': %s", job.video_path,
					                  FFmpegError(status));
				}
				break;
			}
		}
	}

	void ValidateTolerance(double target_timestamp, double decoded_timestamp) const {
		const auto distance = std::fabs(target_timestamp - decoded_timestamp);
		if (distance > bind_data.tolerance) {
			throw InvalidInputException("No frame in LeRobot video '%s' matched timestamp %.6f within tolerance %.6f "
			                            "(closest decoded timestamp %.6f, distance %.6f)",
			                            job.video_path, target_timestamp, bind_data.tolerance, decoded_timestamp,
			                            distance);
		}
	}

	void ConvertFrame(const AVFrame &source, double decoded_timestamp, idx_t target_index, DecodedVideoFrame &result) {
		const auto target_width = bind_data.width > 0 ? bind_data.width : source.width;
		const auto target_height = bind_data.height > 0 ? bind_data.height : source.height;
		if (source.width <= 0 || source.height <= 0 || target_width <= 0 || target_height <= 0) {
			throw InvalidInputException("FFmpeg returned invalid dimensions for LeRobot video '%s'", job.video_path);
		}
		const uint64_t source_byte_count =
		    static_cast<uint64_t>(source.width) * static_cast<uint64_t>(source.height) * 3;
		const uint64_t target_byte_count =
		    static_cast<uint64_t>(target_width) * static_cast<uint64_t>(target_height) * 3;
		if (source_byte_count > static_cast<uint64_t>(std::numeric_limits<idx_t>::max()) ||
		    target_byte_count > static_cast<uint64_t>(std::numeric_limits<idx_t>::max())) {
			throw OutOfMemoryException("Decoded LeRobot video frame is too large");
		}

		const auto source_format = static_cast<AVPixelFormat>(source.format);
		if (!sws_context || sws_source_width != source.width || sws_source_height != source.height ||
		    sws_source_format != source_format) {
			if (sws_context) {
				sws_freeContext(sws_context);
			}
			// PyAV first converts to RGB24 at the native dimensions. Daft then applies
			// Pillow's nearest-neighbour resize as a separate operation.
			sws_context = sws_getContext(source.width, source.height, source_format, source.width, source.height,
			                             AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
			if (!sws_context) {
				throw InvalidInputException("FFmpeg cannot convert LeRobot video '%s' from pixel format %d to RGB24",
				                            job.video_path, static_cast<int>(source_format));
			}
			sws_source_width = source.width;
			sws_source_height = source.height;
			sws_source_format = source_format;
		}

		const bool resize = target_width != source.width || target_height != source.height;
		string &rgb_pixels = resize ? source_rgb_pixels : result.pixels;
		rgb_pixels.resize(static_cast<idx_t>(source_byte_count));
		uint8_t *destination_data[4] = {reinterpret_cast<uint8_t *>(&rgb_pixels[0]), nullptr, nullptr, nullptr};
		int destination_linesize[4] = {source.width * 3, 0, 0, 0};
		const auto scaled_height = sws_scale(sws_context, source.data, source.linesize, 0, source.height,
		                                     destination_data, destination_linesize);
		if (scaled_height != source.height) {
			throw IOException("FFmpeg converted only %d of %d rows for LeRobot video '%s'", scaled_height,
			                  source.height, job.video_path);
		}

		if (resize) {
			if (resize_source_width != source.width || resize_source_height != source.height ||
			    resize_target_width != target_width || resize_target_height != target_height) {
				resize_x_indices.resize(target_width);
				resize_y_indices.resize(target_height);
				const auto horizontal_scale =
				    static_cast<double>(static_cast<float>(source.width)) / static_cast<double>(target_width);
				const auto vertical_scale =
				    static_cast<double>(static_cast<float>(source.height)) / static_cast<double>(target_height);
				double source_x = horizontal_scale * 0.5;
				for (int32_t target_x = 0; target_x < target_width; target_x++) {
					resize_x_indices[target_x] = std::min<int32_t>(source.width - 1, static_cast<int32_t>(source_x));
					source_x += horizontal_scale;
				}
				double source_y = vertical_scale * 0.5;
				for (int32_t target_y = 0; target_y < target_height; target_y++) {
					resize_y_indices[target_y] = std::min<int32_t>(source.height - 1, static_cast<int32_t>(source_y));
					source_y += vertical_scale;
				}
				resize_source_width = source.width;
				resize_source_height = source.height;
				resize_target_width = target_width;
				resize_target_height = target_height;
			}
			result.pixels.resize(static_cast<idx_t>(target_byte_count));
			for (int32_t target_y = 0; target_y < target_height; target_y++) {
				const auto source_y = resize_y_indices[target_y];
				for (int32_t target_x = 0; target_x < target_width; target_x++) {
					const auto source_x = resize_x_indices[target_x];
					const auto source_offset =
					    (static_cast<idx_t>(source_y) * static_cast<idx_t>(source.width) + source_x) * 3;
					const auto target_offset =
					    (static_cast<idx_t>(target_y) * static_cast<idx_t>(target_width) + target_x) * 3;
					std::memcpy(&result.pixels[target_offset], &source_rgb_pixels[source_offset], 3);
				}
			}
		}
		result.target_index = target_index;
		result.decoded_timestamp = decoded_timestamp;
		result.width = target_width;
		result.height = target_height;
	}

	ClientContext &context;
	const LerobotVideoFramesBindData &bind_data;
	const LerobotDecodeJob &job;
	DuckDBAVIOState io_state;
	AVFormatContext *format_context;
	AVIOContext *avio_context;
	AVCodecContext *codec_context;
	AVPacket *packet;
	AVFrame *previous_frame;
	AVFrame *current_frame;
	AVStream *video_stream;
	SwsContext *sws_context;
	idx_t cluster_position;
	idx_t target_position;
	idx_t decoded_frames_since_target;
	bool demux_eof;
	bool flush_sent;
	bool decoder_eof;
	bool have_previous;
	bool have_current;
	double previous_timestamp;
	double current_timestamp;
	string source_rgb_pixels;
	int sws_source_width;
	int sws_source_height;
	AVPixelFormat sws_source_format;
	vector<int32_t> resize_x_indices;
	vector<int32_t> resize_y_indices;
	int resize_source_width;
	int resize_source_height;
	int resize_target_width;
	int resize_target_height;
};

#endif

struct LerobotVideoFramesLocalState final : public LocalTableFunctionState {
#ifdef LEROBOT_HAVE_FFMPEG
	unique_ptr<LerobotClusterDecoder> decoder;
#endif
	DecodedVideoFrame decoded;
};

unique_ptr<LocalTableFunctionState> LerobotVideoFramesInitLocal(ExecutionContext &, TableFunctionInitInput &,
                                                                GlobalTableFunctionState *) {
	return make_uniq<LerobotVideoFramesLocalState>();
}

void WriteDecodedFrame(const LerobotVideoFramesBindData &bind_data, const DecodedVideoFrame &decoded, idx_t row,
                       DataChunk &output) {
	const auto &target = bind_data.targets[decoded.target_index];
	const auto &route = bind_data.routes[target.route_index];
	output.data[0].SetValue(row, Value::BIGINT(target.episode_index));
	output.data[1].SetValue(row, Value::BIGINT(target.frame_index));
	output.data[2].SetValue(row, Value::DOUBLE(target.frame_timestamp));
	output.data[3].SetValue(row, Value(bind_data.metadata->GetVideoKey(route)));
	output.data[4].SetValue(row, Value(bind_data.metadata->GetVideoFile(route)));
	output.data[5].SetValue(row, Value::DOUBLE(target.video_timestamp));
	output.data[6].SetValue(row, Value::DOUBLE(decoded.decoded_timestamp));
	output.data[7].SetValue(row, Value::INTEGER(decoded.width));
	output.data[8].SetValue(row, Value::INTEGER(decoded.height));
	output.data[9].SetValue(row, Value::INTEGER(3));
	output.data[10].SetValue(
	    row, Value::BLOB(reinterpret_cast<const_data_ptr_t>(decoded.pixels.data()), decoded.pixels.size()));
}

void LerobotVideoFramesFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &bind_data = input.bind_data->Cast<LerobotVideoFramesBindData>();
	auto &global_state = input.global_state->Cast<LerobotVideoFramesGlobalState>();
	auto &local_state = input.local_state->Cast<LerobotVideoFramesLocalState>();
	idx_t count = 0;

#ifndef LEROBOT_HAVE_FFMPEG
	if (!bind_data.jobs.empty()) {
		throw MissingExtensionException(
		    "lerobot_video_frames requires FFmpeg development libraries at extension build time; rebuild lerobot "
		    "with LEROBOT_ENABLE_FFMPEG=ON");
	}
#else
	while (count < bind_data.output_batch_size) {
		if (!local_state.decoder) {
			idx_t job_index;
			if (!global_state.ClaimJob(job_index)) {
				break;
			}
			local_state.decoder = make_uniq<LerobotClusterDecoder>(context, bind_data, bind_data.jobs[job_index]);
		}
		if (!local_state.decoder->Next(local_state.decoded)) {
			local_state.decoder.reset();
			continue;
		}
		WriteDecodedFrame(bind_data, local_state.decoded, count, output);
		count++;
	}
#endif
	SetOutputCardinality(output, count, 0);
}

} // namespace

TableFunctionSet LerobotFunctions::GetVideoFramesFunction() {
	TableFunction function("lerobot_video_frames", {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::BIGINT)},
	                       LerobotVideoFramesFunction, LerobotVideoFramesBind, LerobotVideoFramesInitGlobal,
	                       LerobotVideoFramesInitLocal);
	function.named_parameters["video_keys"] = LogicalType::LIST(LogicalType::VARCHAR);
	function.named_parameters["frame_indices"] = LogicalType::LIST(LogicalType::BIGINT);
	function.named_parameters["width"] = LogicalType::BIGINT;
	function.named_parameters["height"] = LogicalType::BIGINT;
	function.named_parameters["tolerance"] = LogicalType::DOUBLE;
	function.named_parameters["cluster_gap"] = LogicalType::DOUBLE;
	function.named_parameters["batch_size"] = LogicalType::BIGINT;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
	return TableFunctionSet(std::move(function));
}

} // namespace duckdb
