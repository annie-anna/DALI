// Copyright (c) 2021-2024, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dali/operators/video/frames_decoder_base.h"
#include <iomanip>
#include <memory>
#include "dali/core/error_handling.h"
#include "dali/core/small_vector.h"
#include "dali/operators/video/video_utils.h"

namespace dali {

int MemoryVideoFile::Read(unsigned char *buffer, int buffer_size) {
  int left_in_file = size_ - position_;
  if (left_in_file <= 0) {
    return AVERROR_EOF;
  }

  int to_read = std::min(left_in_file, buffer_size);
  std::copy(data_ + position_, data_ + position_ + to_read, buffer);
  position_ += to_read;
  return to_read;
}

/**
 * @brief Method for seeking the memory video. It sets position according to provided arguments.
 *
 * @param new_position Requested new_position.
 * @param mode Chosen method of seeking. This argument changes how new_position is interpreted and
 * how seeking is performed.
 * @return int64_t actual new position in the file.
 */
int64_t MemoryVideoFile::Seek(int64_t new_position, int mode) {
  switch (mode) {
    case SEEK_SET:
      position_ = new_position;
      break;
    case AVSEEK_SIZE:
      return size_;

    default:
      DALI_FAIL(make_string(
          "Unsupported seeking method in FramesDecoderBase from memory file. Seeking method: ",
          mode));
  }

  return position_;
}

namespace detail {

int read_memory_video_file(void *data_ptr, uint8_t *av_io_buffer, int av_io_buffer_size) {
  MemoryVideoFile *memory_video_file = static_cast<MemoryVideoFile *>(data_ptr);
  return memory_video_file->Read(av_io_buffer, av_io_buffer_size);
}

int64_t seek_memory_video_file(void *data_ptr, int64_t new_position, int origin) {
  MemoryVideoFile *memory_video_file = static_cast<MemoryVideoFile *>(data_ptr);
  return memory_video_file->Seek(new_position, origin);
}

}  // namespace detail

int FramesDecoderBase::OpenFile(const std::string& filename) {
  LOG_LINE << "Opening file " << filename << std::endl;
  ctx_.reset(avformat_alloc_context());
  DALI_ENFORCE(ctx_, "Could not alloc avformat context");

  int ret = avformat_open_input(&ctx_, filename.c_str(), nullptr, nullptr);
  if (ret < 0) {
    ctx_.reset();
  }
  return ret;
}

int FramesDecoderBase::OpenMemoryFile(MemoryVideoFile &memory_video_file) {
  LOG_LINE << "Opening memory file" << std::endl;
  ctx_.reset(avformat_alloc_context());
  DALI_ENFORCE(ctx_, "Could not alloc avformat context");

  static constexpr int DEFAULT_AV_BUFFER_SIZE = (1 << 15);
  uint8_t* buffer = static_cast<uint8_t*>(av_malloc(DEFAULT_AV_BUFFER_SIZE));
  DALI_ENFORCE(buffer, "Could not alloc avio context buffer");

  auto avio_ctx = avio_alloc_context(
    buffer,
    DEFAULT_AV_BUFFER_SIZE,
    0,
    &memory_video_file,
    detail::read_memory_video_file,
    nullptr,
    detail::seek_memory_video_file);

  if (!avio_ctx) {
    av_freep(&buffer);
    DALI_FAIL("Could not alloc avio context");
  }

  ctx_->pb = avio_ctx;

  int ret = avformat_open_input(&ctx_, "", nullptr, nullptr);
  if (ret < 0) {
    DestroyAvObject(&avio_ctx);
    ctx_.reset();
  }
  return ret;
}

int64_t FramesDecoderBase::NumFrames() {
  if (num_frames_ >= 0) {
    return num_frames_;
  }

  if (ctx_->streams[stream_id_]->nb_frames > 0) {
    num_frames_ = ctx_->streams[stream_id_]->nb_frames;
    return num_frames_;
  }

  ParseNumFrames();
  return num_frames_;
}

std::string FramesDecoderBase::GetAllStreamInfo() const {
  std::stringstream ss;
  ss << "Number of streams: " << ctx_->nb_streams << std::endl;
  for (size_t i = 0; i < ctx_->nb_streams; ++i) {
    ss << "Stream " << i << ": " << ctx_->streams[i]->codecpar->codec_type << std::endl;
    ss << "  Codec ID: " << ctx_->streams[i]->codecpar->codec_id << " ("
       << avcodec_get_name(ctx_->streams[i]->codecpar->codec_id) << ")" << std::endl;
    ss << "  Codec Type: " << ctx_->streams[i]->codecpar->codec_type << std::endl;
    ss << "  Format: " << ctx_->streams[i]->codecpar->format << std::endl;
    ss << "  Width: " << ctx_->streams[i]->codecpar->width << std::endl;
    ss << "  Height: " << ctx_->streams[i]->codecpar->height << std::endl;
    ss << "  Sample Rate: " << ctx_->streams[i]->codecpar->sample_rate << std::endl;
    ss << "  Bit Rate: " << ctx_->streams[i]->codecpar->bit_rate << std::endl;
  }
  return ss.str();
}

bool FramesDecoderBase::SelectVideoStream(int stream_id) {
  if (stream_id < 0) {
    LOG_LINE << "Finding video stream" << std::endl;
    stream_id = av_find_best_stream(ctx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_id == AVERROR_STREAM_NOT_FOUND) {
      DALI_WARN(make_string("Could not find a valid video stream in a file in ", Filename()));
      return false;
    }
  }
  if (stream_id < 0 || stream_id >= static_cast<int>(ctx_->nb_streams)) {
    LOG_LINE << "Invalid stream id: " << stream_id << std::endl;
    return false;
  }
  stream_id_ = stream_id;
  codec_params_ = ctx_->streams[stream_id_]->codecpar;
  LOG_LINE << "Selecting stream " << stream_id_
           << " (codec_id=" << codec_params_->codec_id
           << ", codec_type=" << codec_params_->codec_type
           << ", format=" << codec_params_->format
           << ", width=" << codec_params_->width
           << ", height=" << codec_params_->height
           << ", sample_rate=" << codec_params_->sample_rate
           << ", bit_rate=" << codec_params_->bit_rate << ")" << std::endl;

  assert(codec_params_->codec_type != AVMEDIA_TYPE_NB);
  switch (codec_params_->codec_type) {
    case AVMEDIA_TYPE_UNKNOWN:  // if unknown, we can't determine if it's a video stream
    case AVMEDIA_TYPE_VIDEO:
      break;
    case AVMEDIA_TYPE_AUDIO:  // fall through
    case AVMEDIA_TYPE_DATA:   // fall through
    case AVMEDIA_TYPE_SUBTITLE:  // fall through
    case AVMEDIA_TYPE_ATTACHMENT:  // fall through
    default:
      LOG_LINE << "Stream " << stream_id << " is not a video stream" << std::endl;
      codec_params_ = nullptr;
      stream_id_ = -1;
      return false;
  }
  LOG_LINE << "Selected stream " << stream_id << " with codec "
           << avcodec_get_name(codec_params_->codec_id) << " ("
           << codec_params_->codec_id << ")" << std::endl;
  if (!CheckDimensions())
    return false;

  next_frame_idx_ = 0;
  can_seek_ = true;
  return true;
}

bool FramesDecoderBase::CheckDimensions() {
  if (Height() == 0 || Width() == 0) {
    if (avformat_find_stream_info(ctx_, nullptr) < 0) {
      DALI_WARN(make_string("Could not find stream information in ", Filename()));
      return false;
    }
    if (Height() == 0 || Width() == 0) {
      DALI_WARN("Couldn't load video size info.");
      return false;
    }
  }
  return true;
}

FramesDecoderBase::FramesDecoderBase(const std::string &filename, DALIImageType image_type) {
  av_log_set_level(AV_LOG_ERROR);
  filename_ = filename;
  DALI_ENFORCE(image_type == DALI_YCbCr || image_type == DALI_RGB,
               make_string("Invalid image type: ", image_type));
  image_type_ = image_type;
  int ret = OpenFile(filename);
  if (ret < 0) {
    DALI_WARN(make_string("Failed to open video file \"", Filename(), "\", due to ",
                          av_error_string(ret)));
    return;
  }

  packet_.reset(av_packet_alloc());
  DALI_ENFORCE(packet_, "Could not allocate av packet");

  is_valid_ = true;
  can_seek_ = true;
  next_frame_idx_ = 0;
}

FramesDecoderBase::FramesDecoderBase(const char *memory_file, size_t memory_file_size, std::string_view source_info, DALIImageType image_type) {
  av_log_set_level(AV_LOG_ERROR);
  filename_ = source_info;
  DALI_ENFORCE(image_type == DALI_YCbCr || image_type == DALI_RGB,
               make_string("Invalid image type: ", image_type));
  image_type_ = image_type;
  memory_video_file_ = std::make_unique<MemoryVideoFile>(memory_file, memory_file_size);
  int ret = OpenMemoryFile(*memory_video_file_);
  if (ret < 0) {
    DALI_WARN(make_string("Failed to open video file from memory buffer due to: ",
                          av_error_string(ret)));
    return;
  }

  packet_.reset(av_packet_alloc());
  DALI_ENFORCE(packet_, "Could not allocate av packet");

  is_valid_ = true;
  can_seek_ = true;
  next_frame_idx_ = 0;
}

void FramesDecoderBase::ParseNumFrames() {
  num_frames_ = 0;
  while (true) {
    int ret = av_read_frame(ctx_, packet_);
    auto packet = AVPacketScope(packet_, av_packet_unref);
    if (ret != 0) {
      break;  // End of file
    }

    if (packet->stream_index != stream_id_) {
      continue;
    }
    ++num_frames_;
  }
  Reset();
}

/**
 * @brief Reads the length of a Network Abstraction Layer (NAL) unit from a buffer.
 *
 * NAL units are the basic elements of H.264/AVC and H.265/HEVC video compression standards.
 * In the Annex B byte stream format, NAL units are prefixed with a 4-byte length field
 * that indicates the size of the following NAL unit.
 *
 * This function reads those 4 bytes in big-endian order to get the NAL unit length.
 *
 * Reference: ITU-T H.264 and H.265 specifications
 *
 * @param buf Pointer to the buffer containing the NAL unit length prefix
 * @return The length of the NAL unit in bytes
 */
static inline uint32_t read_nal_unit_length(uint8_t *buf) {
  uint32_t length = 0;
  length = (buf)[0] << 24 | (buf)[1] << 16 | (buf)[2] << 8 | (buf)[3];
  return length;
}

void FramesDecoderBase::BuildIndex() {
  if (HasIndex()) {
    return;
  }

  // Initialize frame index
  index_.index.clear();
  index_.filename = Filename();
  index_.timebase = ctx_->streams[stream_id_]->time_base;

  // Track the position of the last keyframe seen
  int last_keyframe = -1;
  int frame_count = 0;

  while (true) {
    // Read the next frame from the video
    int ret = av_read_frame(ctx_, packet_);
    auto packet = AVPacketScope(packet_, av_packet_unref);
    if (ret != 0) {
      LOG_LINE << "End of file reached after " << frame_count << " frames" << std::endl;
      break;  // Just break when we hit EOF instead of trying to seek back
    }

    frame_count++;

    // Skip packets from other streams (e.g. audio)
    if (packet->stream_index != stream_id_) {
      continue;
    }

    IndexEntry entry;
    entry.is_keyframe = false;  // Default to false, set true only if confirmed

    // Check if this packet contains a keyframe
    if (packet->flags & AV_PKT_FLAG_KEY) {
      LOG_LINE << "Found potential keyframe at frame " << index_.size() << std::endl;

      // Special handling for H.264 and HEVC formats
      auto codec_id = ctx_->streams[packet->stream_index]->codecpar->codec_id;
      if (codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC) {
        // Parse NAL units to verify if this is actually a keyframe
        // NAL = Network Abstraction Layer, the basic unit of encoded video
        const uint8_t *end = packet->data + packet->size;
        uint8_t *nal_start = packet->data;

        // Iterate through NAL units in the packet
        while (nal_start + 4 < end) {
          // Each NAL unit is prefixed with a 4-byte length
          uint32_t nal_size = read_nal_unit_length(nal_start);
          nal_start += 4;
          if (nal_start + nal_size > end) {
            break;
          }

          if (codec_id == AV_CODEC_ID_H264) {
            // In H.264, the NAL unit type is in the lower 5 bits
            uint8_t nal_unit_type = nal_start[0] & 0x1F;
            // Type 5 indicates an IDR frame (Instantaneous Decoding Refresh)
            // IDR frames are special keyframes that clear all reference buffers
            if (nal_unit_type == 5) {
              entry.is_keyframe = true;
              break;
            }
          } else {  // AV_CODEC_ID_HEVC
            // In HEVC/H.265, NAL unit type is in bits 1-6 of the first byte
            uint8_t nal_unit_type = (nal_start[0] >> 1) & 0x3F;
            // Types 16-21 are IRAP (Intra Random Access Point) pictures
            // which serve as keyframes in HEVC
            if (nal_unit_type >= 16 && nal_unit_type <= 21) {
              entry.is_keyframe = true;
              break;
            }
          }
          nal_start += nal_size;  // Advance to next NAL unit
        }
      } else {
        // For other codecs, trust the AV_PKT_FLAG_KEY flag
        entry.is_keyframe = true;
      }
    }

    // Store presentation timestamp (pts) or decode timestamp (dts) if pts not available
    entry.pts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
    if (entry.pts == AV_NOPTS_VALUE) {
      DALI_FAIL(make_string("Video file \"", Filename(), "\" has no valid timestamps"));
    }

    if (entry.pts < 0) {
      LOG_LINE << "Negative timestamp: " << entry.pts << ", skipping" << std::endl;
      continue;
    }

    // Update last keyframe position if this is a keyframe
    if (entry.is_keyframe) {
      last_keyframe = index_.size();
    }
    entry.last_keyframe_id = last_keyframe;

    // Regular frame, not a flush frame
    entry.is_flush_frame = false;
    index_.index.push_back(entry);
    num_frames_ = index_.size();
  }

  LOG_LINE << "Index building complete. Total frames: " << index_.size() << std::endl;

  DALI_ENFORCE(index_.index.size() > 0,
               make_string("No valid frames found in video file \"", Filename(), "\""));

  // Mark last frame as flush frame
  index_.index.back().is_flush_frame = true;

  // Sort frames by presentation timestamp
  // This is needed because frames may be stored out of order in the container
  std::sort(index_.index.begin(), index_.index.end(),
            [](const IndexEntry &a, const IndexEntry &b) { return a.pts < b.pts; });

  // After sorting, we need to update last_keyframe_id references
  std::vector<int> keyframe_positions;
  for (size_t i = 0; i < index_.size(); i++) {
    if (index_[i].is_keyframe) {
      keyframe_positions.push_back(i);
    }
  }

  DALI_ENFORCE(!keyframe_positions.empty(),
               make_string("No keyframes found in video file \"", Filename(), "\""));

  // Update last_keyframe_id for each frame after sorting
  for (size_t i = 0; i < index_.size(); i++) {
    // Find the last keyframe that comes before or at this frame
    auto it = std::upper_bound(keyframe_positions.begin(), keyframe_positions.end(), i);
    if (it == keyframe_positions.begin()) {
      index_[i].last_keyframe_id = 0;  // First keyframe
    } else {
      index_[i].last_keyframe_id = *(--it);
    }
  }

  // Detect if video has variable frame rate (VFR)
  DetectVariableFrameRate();
  Reset();
}

void FramesDecoderBase::DetectVariableFrameRate() {
  is_vfr_ = false;
  if (index_.size() > 3) {
    int64_t pts_step = index_[1].pts - index_[0].pts;
    for (size_t i = 2; i < index_.size(); i++) {
      if (index_[i].pts - index_[i-1].pts != pts_step) {
        is_vfr_ = true;
        break;
      }
    }
  }
}

bool FramesDecoderBase::AvSeekFrame(int64_t timestamp, int frame_id) {
  if (!can_seek_) {
    LOG_LINE << "Not seekable, returning directly" << std::endl;
    return false;
  }

  can_seek_ =
      av_seek_frame(ctx_, stream_id_, timestamp, AVSEEK_FLAG_BACKWARD) >= 0;
  if (!can_seek_)
    return false;

  LOG_LINE << "Seeked to frame " << frame_id << std::endl;
  Flush();

  next_frame_idx_ = frame_id;
  return true;
}

void FramesDecoderBase::Reset() {
  LOG_LINE << "Reset: Reopening stream." << std::endl;
  int stream_id = stream_id_;

  int ret = -1;
  if (memory_video_file_) {
    memory_video_file_->Seek(0, SEEK_SET);
    ret = OpenMemoryFile(*memory_video_file_);
    DALI_ENFORCE(ret >= 0,
                 make_string("Could not open video file from memory buffer due to: ",
                             av_error_string(ret)));
  } else {
    ret = OpenFile(Filename());
    DALI_ENFORCE(ret >= 0,
                 make_string("Could not open video file \"", Filename(),
                    "\" due to: ", av_error_string(ret)));
  }

  is_valid_ = true;
  can_seek_ = true;
  next_frame_idx_ = 0;

  SelectVideoStream(stream_id);
}

void FramesDecoderBase::SeekFrame(int frame_id) {
  LOG_LINE << "SeekFrame: Seeking to frame " << frame_id
            << " (current=" << next_frame_idx_ << ")" << std::endl;

  // TODO(awolant): Optimize seeking:
  //  - for CFR, when we know pts, but don't know keyframes
  DALI_ENFORCE(
      frame_id >= 0 && frame_id < NumFrames(),
      make_string("Invalid seek frame id. frame_id = ", frame_id, ", num_frames = ", NumFrames()));

  if (frame_id == next_frame_idx_) {
    LOG_LINE << "Already at requested frame" << std::endl;
    return;  // No need to seek
  }

  if (next_frame_idx_ < 0) {
    Reset();
  }
  assert(next_frame_idx_ >= 0);

  // If we are seeking to a frame that is before the current frame,
  // or we are seeking to a frame that is more than MINIMUM_SEEK_LEAP frames away,
  // or the current frame index is invalid (e.g. end of file),
  // we will to seek to the nearest keyframe first
  LOG_LINE << "SeekFrame: frame_id=" << frame_id << ", next_frame_idx=" << next_frame_idx_
           << std::endl;
  constexpr int MINIMUM_SEEK_LEAP = 10;
  if (frame_id < next_frame_idx_ || frame_id > next_frame_idx_ + MINIMUM_SEEK_LEAP) {
    // If we have an index, we can seek to the nearest keyframe first
    if (HasIndex()) {
      LOG_LINE << "Using index to find nearest keyframe" << std::endl;
      const auto &current_frame = index_[next_frame_idx_];
      const auto &requested_frame = index_[frame_id];
      auto keyframe_id = requested_frame.last_keyframe_id;
      // if we are seeking to a different keyframe than the current frame,
      // or if we are seeking to a frame that is before the current frame,
      // we need to seek to the keyframe first
      LOG_LINE << "current_frame.last_keyframe_id=" << current_frame.last_keyframe_id
               << ", keyframe_id=" << keyframe_id << ", frame_id=" << frame_id
               << ", next_frame_idx_=" << next_frame_idx_ << std::endl;

      if (current_frame.last_keyframe_id != keyframe_id || frame_id < next_frame_idx_) {
        // We are seeking to a different keyframe than the current frame,
        // so we need to seek to the keyframe first
        auto &keyframe_entry = index_[keyframe_id];
        LOG_LINE << "Seeking to key frame " << keyframe_id << " timestamp " << keyframe_entry.pts
                 << " for requested frame " << frame_id << " timestamp " << requested_frame.pts
                 << std::endl;

        if (!AvSeekFrame(keyframe_entry.pts, keyframe_id)) {
          LOG_LINE << "Failed to seek to keyframe " << keyframe_id << " timestamp "
                   << keyframe_entry.pts << ". Resetting decoder." << std::endl;
          Reset();
        }
      }
    } else if (frame_id < next_frame_idx_) {
      LOG_LINE << "No index & seeking backwards. Resetting decoder." << std::endl;
      Reset();
    }
  }
  LOG_LINE << "After seeking: next_frame_idx_=" << next_frame_idx_ << ", frame_id=" << frame_id
           << std::endl;
  assert(next_frame_idx_ <= frame_id);
  // Skip all remaining frames until the requested frame
  LOG_LINE << "Skipping frames from " << next_frame_idx_ << " to " << frame_id << std::endl;
  for (int i = next_frame_idx_; i < frame_id; i++) {
    ReadNextFrame(nullptr);
  }
  LOG_LINE << "After skipping: next_frame_idx_=" << next_frame_idx_ << ", frame_id=" << frame_id
           << std::endl;
  assert(next_frame_idx_ == frame_id);
}

int FramesDecoderBase::HandleBoundary(boundary::BoundaryType boundary_type, int frame_id, int roi_start, int roi_end) {
  DALI_ENFORCE(boundary_type == boundary::BoundaryType::CLAMP ||
                   boundary_type == boundary::BoundaryType::CONSTANT ||
                   boundary_type == boundary::BoundaryType::REFLECT_1001 ||
                   boundary_type == boundary::BoundaryType::REFLECT_101 ||
                   boundary_type == boundary::BoundaryType::ISOLATED,
               make_string("Invalid boundary type: ", boundary::to_string(boundary_type)));
  if (frame_id >= roi_start && frame_id < roi_end) {
    return frame_id;
  }
  switch (boundary_type) {
    case boundary::BoundaryType::CLAMP:
      return std::clamp(frame_id, roi_start, roi_end - 1);
    case boundary::BoundaryType::CONSTANT:
      return -1;
    case boundary::BoundaryType::REFLECT_1001:
      return boundary::idx_reflect_1001(frame_id, roi_end);
    case boundary::BoundaryType::REFLECT_101:
      return boundary::idx_reflect_101(frame_id, roi_end);
    case boundary::BoundaryType::ISOLATED:
    default:
      DALI_FAIL(make_string(
          "Unexpected out-of-bounds frame index ", frame_id,
          " for pad_mode = 'none' and a sample containing a ROI with ", roi_end - roi_start,
          " frames. Range of valid frame indices for this sample is [", roi_start, ", ", roi_end,
          "). Change `pad_mode` to other than 'none' "
          "for out-of-bounds sampling."));
  }
}

void FramesDecoderBase::DecodeFramesImpl(uint8_t *data,
                                         SmallVector<std::pair<int, int>, 32> frame_ids,
                                         boundary::BoundaryType boundary_type,
                                         const uint8_t *constant_frame,
                                         span<double> out_timestamps) {
  DALI_ENFORCE(constant_frame != nullptr || boundary_type != boundary::BoundaryType::CONSTANT,
               make_string("Constant frame must be provided if boundary type is CONSTANT"));

  int last_out_frame_idx = -1;
  for (auto &[frame_id, i] : frame_ids) {
    if (frame_id >= 0 && frame_id < NumFrames()) {
      LOG_LINE << "Decoding frame " << frame_id << " to position " << i << std::endl;
      SeekFrame(frame_id);
      ReadNextFrame(data + i * FrameSize());
      last_out_frame_idx = i;
    } else if (frame_id < 0) {
      LOG_LINE << "Copying constant frame to position " << i << std::endl;
      CopyFrame(data + i * FrameSize(), constant_frame);
    } else {
      LOG_LINE << "Copying last decoded frame " << last_out_frame_idx << " to position " << i
               << std::endl;
      CopyFrame(data + i * FrameSize(), data + last_out_frame_idx * FrameSize());
    }
  }

  if (!out_timestamps.empty()) {
    LOG_LINE << "Computing timestamps for " << out_timestamps.size() << " frames" << std::endl;
    int64_t pts_0 = index_[0].pts;
    for (auto& [frame_idx, i] : frame_ids) {
      if (frame_idx >= 0) {
        out_timestamps[i] = TimestampToSeconds(GetTimebase(), index_[frame_idx].pts - pts_0);
      } else {
        out_timestamps[i] = -1.0f;
      }
    }
  }
}

void FramesDecoderBase::DecodeFrames(uint8_t *data, span<const int> frame_ids,
                                     boundary::BoundaryType boundary_type,
                                     const uint8_t *constant_frame,
                                     span<double> out_timestamps) {
  LOG_LINE << "DecodeFrames: " << frame_ids.size() << " frames, boundary_type="
           << boundary::to_string(boundary_type) << std::endl;

  SmallVector<std::pair<int, int>, 32> sorted_frame_ids;
  size_t num_frames = frame_ids.size();
  sorted_frame_ids.reserve(num_frames);
  for (int i = 0; i < static_cast<int>(frame_ids.size()); i++) {
    sorted_frame_ids.push_back({HandleBoundary(boundary_type, frame_ids[i], 0, NumFrames()), i});
  }
  std::sort(sorted_frame_ids.begin(), sorted_frame_ids.end());
  DecodeFramesImpl(data, sorted_frame_ids, boundary_type, constant_frame, out_timestamps);
}


void FramesDecoderBase::DecodeFrames(uint8_t *data, int start_frame, int end_frame, int stride,
                                     boundary::BoundaryType boundary_type,
                                     const uint8_t *constant_frame,
                                     span<double> out_timestamps) {
  LOG_LINE << "DecodeFrames: start=" << start_frame << ", end=" << end_frame
           << ", stride=" << stride << std::endl;

  SmallVector<std::pair<int, int>, 32> sorted_frame_ids;
  size_t num_frames = (end_frame - start_frame + stride - 1) / stride;
  sorted_frame_ids.reserve(num_frames);
  for (int i = 0; i < static_cast<int>(num_frames); i++) {
    sorted_frame_ids.push_back(
        {HandleBoundary(boundary_type, start_frame + i * stride, 0, NumFrames()), i});
  }
  std::sort(sorted_frame_ids.begin(), sorted_frame_ids.end());
  DecodeFramesImpl(data, sorted_frame_ids, boundary_type, constant_frame, out_timestamps);
}

}  // namespace dali
