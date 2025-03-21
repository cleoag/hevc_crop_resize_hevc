#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>           // For UCHAR_MAX
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libswscale/swscale.h>
#include <x265.h>            // x265 encoder

// Configuration parameters
#define INPUT_WIDTH 5760     // Input stereo width
#define INPUT_HEIGHT 2880    // Input height
#define OUTPUT_WIDTH 200     // Output width
#define OUTPUT_HEIGHT 200    // Output height (square)
#define FRAME_RATE 50        // Output frame rate
#define OUTPUT_TIMEBASE 48000 // Output timebase denominator
#define MAX_NAL_SIZE (4*1024*1024)  // 4MB buffer for NAL units

// Enable fMP4 muxing
#define ENABLE_MP4_MUXING 1  // Set to 1 to output fMP4, 0 for raw HEVC

typedef struct {
    // Libav decoder
    AVCodec *decoder_codec;
    AVCodecContext *decoder_ctx;
    AVFormatContext *fmt_ctx;
    int video_stream_idx;
    AVFrame *frame;
    AVPacket *pkt;
    
    // Crop and scale
    struct SwsContext *sws_ctx;
    uint8_t *scaled_buffer;
    
    // x265 encoder
    x265_encoder *encoder;
    x265_param *encoder_params;
    x265_picture *enc_pic;
    
    // File I/O for raw HEVC
    FILE *output_file;
    
    // Muxing output to MP4
    AVFormatContext *ofmt_ctx;
    AVStream *out_stream;
    int64_t pts_offset;
    int64_t next_pts;
    uint8_t *extradata;
    int extradata_size;
    
    // Processing options
    int skip_frames;        // 1 to skip every other frame, 0 to process all frames
    int mp4_output;         // 1 to output MP4, 0 for raw HEVC
} ProcessingContext;

// Initialize x265 encoder with better quality settings
int init_encoder(ProcessingContext *ctx) {
    // Allocate param structure
    ctx->encoder_params = x265_param_alloc();
    if (!ctx->encoder_params) {
        fprintf(stderr, "Failed to allocate encoder parameters\n");
        return -1;
    }
    
    // Set defaults for preset - use 'medium' instead of 'ultrafast' for better quality
    x265_param_default_preset(ctx->encoder_params, "medium", "zerolatency");
    
    // Configure encoder for better quality while maintaining reasonable speed
    ctx->encoder_params->sourceWidth = OUTPUT_WIDTH;
    ctx->encoder_params->sourceHeight = OUTPUT_HEIGHT;
    
    // Set frame rate - this is how x265 defines timebase internally
    int output_fps = ctx->skip_frames ? FRAME_RATE / 2 : FRAME_RATE;
    ctx->encoder_params->fpsNum = output_fps;
    ctx->encoder_params->fpsDenom = 1;
    
    // Note: x265 doesn't have a direct timebase parameter, it derives it from fps
    
    ctx->encoder_params->internalCsp = X265_CSP_I420;
    
    // Quality settings
    ctx->encoder_params->bframes = 3;                // Allow B-frames for better compression
    ctx->encoder_params->maxNumReferences = 3;       // More reference frames for better quality
    ctx->encoder_params->rc.bitrate = 3000;          // 3 Mbps for better quality
    ctx->encoder_params->rc.qpMin = 17;              // Lower minimum QP for higher quality
    ctx->encoder_params->rc.qpMax = 37;              // Lower maximum QP for better quality
    ctx->encoder_params->rc.rateControlMode = X265_RC_ABR; // Average bitrate mode
    
    // Performance settings - utilize more CPU for better quality
    ctx->encoder_params->frameNumThreads = 4;        // Use multiple threads per frame
    ctx->encoder_params->bEnableWavefront = 1;       // Enable wavefront parallel processing
    ctx->encoder_params->lookaheadDepth = 20;        // Increased lookahead for better rate control
    
    // Set HEVC profile and level for compatibility
    ctx->encoder_params->bRepeatHeaders = 1;         // Include headers with each keyframe
    ctx->encoder_params->bEmitHRDSEI = 1;            // Emit HRD info for better compatibility
    ctx->encoder_params->keyframeMin = 1;            // Minimum GOP size
    ctx->encoder_params->keyframeMax = 120;          // Maximum GOP size
    ctx->encoder_params->bOpenGOP = 0;               // Closed GOP structure
    ctx->encoder_params->levelIdc = 0;               // Let x265 choose level
    
    // Psychovisual optimizations for better perceived quality
    ctx->encoder_params->psyRd = 1.0;                // Psychovisual rate-distortion optimization
    ctx->encoder_params->psyRdoq = 1.0;              // Psychovisual optimization in quantization
    
    // Create encoder
    ctx->encoder = x265_encoder_open(ctx->encoder_params);
    if (!ctx->encoder) {
        fprintf(stderr, "Failed to open x265 encoder\n");
        x265_param_free(ctx->encoder_params);
        return -1;
    }
    
    // Allocate picture
    ctx->enc_pic = x265_picture_alloc();
    x265_picture_init(ctx->encoder_params, ctx->enc_pic);
    
    return 0;
}

// Prepare frame for x265 encoding
void prepare_for_encoding(ProcessingContext *ctx, int64_t pts) {
    // Set plane pointers
    ctx->enc_pic->planes[0] = ctx->scaled_buffer;  // Y plane
    ctx->enc_pic->planes[1] = ctx->scaled_buffer + (OUTPUT_WIDTH * OUTPUT_HEIGHT);  // U plane
    ctx->enc_pic->planes[2] = ctx->scaled_buffer + (OUTPUT_WIDTH * OUTPUT_HEIGHT) + (OUTPUT_WIDTH * OUTPUT_HEIGHT / 4);  // V plane
    
    // Set stride values
    ctx->enc_pic->stride[0] = OUTPUT_WIDTH;
    ctx->enc_pic->stride[1] = OUTPUT_WIDTH / 2;
    ctx->enc_pic->stride[2] = OUTPUT_WIDTH / 2;
    
    // Set other picture properties
    ctx->enc_pic->pts = pts;
    ctx->enc_pic->bitDepth = 8;
    ctx->enc_pic->colorSpace = X265_CSP_I420;
}

// Initialize MP4 muxer
int init_mp4_muxer(ProcessingContext *ctx, const char *output_file) {
    int ret;
    avformat_alloc_output_context2(&ctx->ofmt_ctx, NULL, "mp4", output_file);
    if (!ctx->ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        return -1;
    }
    
    // Add video stream
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    ctx->out_stream = avformat_new_stream(ctx->ofmt_ctx, codec);
    if (!ctx->out_stream) {
        fprintf(stderr, "Failed to allocate output stream\n");
        return -1;
    }
    
    ctx->out_stream->id = ctx->ofmt_ctx->nb_streams - 1;
    
    // Configure stream parameters
    AVCodecParameters *codecpar = ctx->out_stream->codecpar;
    codecpar->codec_id = AV_CODEC_ID_HEVC;
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->width = OUTPUT_WIDTH;
    codecpar->height = OUTPUT_HEIGHT;
    codecpar->format = AV_PIX_FMT_YUV420P;
    codecpar->bit_rate = ctx->encoder_params->rc.bitrate * 1000;
    
    // Set stream timebase - for MP4, must match the timebase we use for timestamps
    ctx->out_stream->time_base = (AVRational){1, OUTPUT_TIMEBASE};
    
    // Set fragmented MP4 options
    if (ctx->ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        ctx->ofmt_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    
    // Set MP4 fragmentation options for lower latency streaming
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    
    // Open output file
    if (!(ctx->ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ctx->ofmt_ctx->pb, output_file, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'\n", output_file);
            return -1;
        }
    }
    
    // Save initial x265 header info for extradata
    ctx->extradata = NULL;
    ctx->extradata_size = 0;
    ctx->pts_offset = 0;
    ctx->next_pts = 0;
    
    // Initialize with invalid PTS to detect the first real frame
    ctx->pts_offset = AV_NOPTS_VALUE;
    
    return 0;
}

// Write x265 NAL units to MP4 container
int write_nals_to_mp4(ProcessingContext *ctx, x265_nal *nals, uint32_t nal_count, int64_t pts, int is_key_frame) {
    if (!nals || nal_count == 0) {
        return 0;
    }
    
    // Create AVPacket using modern API
    AVPacket pkt = {0};
    av_packet_unref(&pkt);
    
    // Calculate total size needed
    int total_size = 0;
    for (uint32_t i = 0; i < nal_count; i++) {
        total_size += nals[i].sizeBytes;
    }
    
    // Allocate buffer for packet data
    uint8_t *packet_data = av_malloc(total_size);
    if (!packet_data) {
        fprintf(stderr, "Failed to allocate packet data buffer\n");
        return -1;
    }
    
    // Copy NAL units into packet buffer
    int offset = 0;
    for (uint32_t i = 0; i < nal_count; i++) {
        memcpy(packet_data + offset, nals[i].payload, nals[i].sizeBytes);
        offset += nals[i].sizeBytes;
    }
    
    // Set up packet
    pkt.data = packet_data;
    pkt.size = total_size;
    pkt.pts = pkt.dts = pts;
    pkt.duration = 0;  // Will be filled in by the muxer
    pkt.stream_index = ctx->out_stream->index;
    pkt.flags = is_key_frame ? AV_PKT_FLAG_KEY : 0;
    
    // Store the latest PTS for duration calculations
    ctx->next_pts = pts;
    
    // Write packet to MP4 container
    int ret = av_interleaved_write_frame(ctx->ofmt_ctx, &pkt);
    if (ret < 0) {
        fprintf(stderr, "Error writing packet to output: %d\n", ret);
        av_free(packet_data);
        av_packet_unref(&pkt);
        return -1;
    }
    
    // Free packet data
    av_free(packet_data);
    av_packet_unref(&pkt);
    
    return 0;
}

// Write HEVC headers (VPS, SPS, PPS) as extradata to MP4
int write_hevc_headers_to_mp4(ProcessingContext *ctx, x265_nal *nals, uint32_t nal_count) {
    if (!nals || nal_count == 0) {
        return 0;
    }
    
    // Calculate total size needed
    int total_size = 0;
    for (uint32_t i = 0; i < nal_count; i++) {
        total_size += nals[i].sizeBytes + 4; // 4 bytes for length prefix
    }
    
    // Allocate buffer for extradata
    ctx->extradata = av_mallocz(total_size);
    if (!ctx->extradata) {
        fprintf(stderr, "Failed to allocate extradata buffer\n");
        return -1;
    }
    
    // Format headers as AVCC format (length-prefixed NALs)
    int offset = 0;
    for (uint32_t i = 0; i < nal_count; i++) {
        uint32_t size = nals[i].sizeBytes;
        ctx->extradata[offset++] = (size >> 24) & 0xFF;
        ctx->extradata[offset++] = (size >> 16) & 0xFF;
        ctx->extradata[offset++] = (size >> 8) & 0xFF;
        ctx->extradata[offset++] = size & 0xFF;
        memcpy(ctx->extradata + offset, nals[i].payload, nals[i].sizeBytes);
        offset += nals[i].sizeBytes;
    }
    
    ctx->extradata_size = offset;
    
    // Set the extradata in the stream codec parameters
    ctx->out_stream->codecpar->extradata = av_mallocz(ctx->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!ctx->out_stream->codecpar->extradata) {
        fprintf(stderr, "Failed to allocate stream extradata\n");
        av_free(ctx->extradata);
        ctx->extradata = NULL;
        return -1;
    }
    
    memcpy(ctx->out_stream->codecpar->extradata, ctx->extradata, ctx->extradata_size);
    ctx->out_stream->codecpar->extradata_size = ctx->extradata_size;
    
    // Write MP4 header
    int ret = avformat_write_header(ctx->ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error writing MP4 header: %d\n", ret);
        return -1;
    }
    
    return 0;
}

// Clean up and free resources
void cleanup(ProcessingContext *ctx) {
    // Free encoder resources
    if (ctx->encoder) {
        x265_encoder_close(ctx->encoder);
    }
    if (ctx->encoder_params) {
        x265_param_free(ctx->encoder_params);
    }
    if (ctx->enc_pic) {
        x265_picture_free(ctx->enc_pic);
    }
    
    // Free decoder resources
    if (ctx->frame) {
        av_frame_free(&ctx->frame);
    }
    if (ctx->pkt) {
        av_packet_free(&ctx->pkt);
    }
    if (ctx->decoder_ctx) {
        avcodec_free_context(&ctx->decoder_ctx);
    }
    if (ctx->fmt_ctx) {
        avformat_close_input(&ctx->fmt_ctx);
    }
    
    // Free scaling context
    if (ctx->sws_ctx) {
        sws_freeContext(ctx->sws_ctx);
    }
    
    // Free buffers
    free(ctx->scaled_buffer);
    
    // Close files
    if (ctx->output_file) {
        fclose(ctx->output_file);
    }
    
    // Close MP4 muxer
    if (ctx->ofmt_ctx) {
        if (ctx->mp4_output && ctx->ofmt_ctx->pb) {
            // Write trailer before closing
            av_write_trailer(ctx->ofmt_ctx);
        }
        
        if (!(ctx->ofmt_ctx->oformat->flags & AVFMT_NOFILE) && ctx->ofmt_ctx->pb) {
            avio_closep(&ctx->ofmt_ctx->pb);
        }
        
        avformat_free_context(ctx->ofmt_ctx);
    }
    
    // Free extradata
    if (ctx->extradata) {
        av_free(ctx->extradata);
    }
}

// Process frame using FFmpeg SwScale for crop and scale in one step
int process_frame_with_swscale(ProcessingContext *ctx, AVFrame *frame) {
    // Set up source planes for cropping - only use left half of the frame
    const uint8_t *src_data[4] = {
        frame->data[0],                   // Y plane source
        frame->data[1],                   // U plane source
        frame->data[2],                   // V plane source
        NULL
    };
    
    // Set up source strides for entire frame
    int src_linesize[4] = {
        frame->linesize[0],               // Y plane stride
        frame->linesize[1],               // U plane stride
        frame->linesize[2],               // V plane stride
        0
    };
    
    // Set up destination planes
    uint8_t *dst_data[4] = {
        ctx->scaled_buffer,                                             // Y plane destination
        ctx->scaled_buffer + (OUTPUT_WIDTH * OUTPUT_HEIGHT),            // U plane destination
        ctx->scaled_buffer + (OUTPUT_WIDTH * OUTPUT_HEIGHT) + 
            (OUTPUT_WIDTH * OUTPUT_HEIGHT / 4),                         // V plane destination
        NULL
    };
    
    // Set up destination strides
    int dst_linesize[4] = {
        OUTPUT_WIDTH,                     // Y plane stride
        OUTPUT_WIDTH / 2,                 // U plane stride
        OUTPUT_WIDTH / 2,                 // V plane stride
        0
    };
    
    // Perform crop and scale in one step
    // The crop is achieved by only using the left half of the input frame as source
    // srcSliceY = 0, srcSliceH = INPUT_HEIGHT means process the whole height
    sws_scale(ctx->sws_ctx, src_data, src_linesize, 0, INPUT_HEIGHT, dst_data, dst_linesize);
    
    return 0;
}

// Init decoder using demuxing API instead of raw NAL parsing
int init_decoder(ProcessingContext *ctx, const char *input_file) {
    int ret;
    
    // Open input file using FFmpeg demuxer
    ret = avformat_open_input(&ctx->fmt_ctx, input_file, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input file '%s'\n", input_file);
        return -1;
    }
    
    // Get stream information
    ret = avformat_find_stream_info(ctx->fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        return -1;
    }
    
    // Find video stream
    ctx->video_stream_idx = -1;
    for (int i = 0; i < ctx->fmt_ctx->nb_streams; i++) {
        if (ctx->fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            ctx->video_stream_idx = i;
            break;
        }
    }
    
    if (ctx->video_stream_idx == -1) {
        fprintf(stderr, "Could not find video stream\n");
        return -1;
    }
    
    // Find decoder - using const AVCodec* as required by newer FFmpeg
    const AVCodec *decoder_codec = avcodec_find_decoder(ctx->fmt_ctx->streams[ctx->video_stream_idx]->codecpar->codec_id);
    if (!decoder_codec) {
        fprintf(stderr, "Failed to find decoder\n");
        return -1;
    }
    
    // Store in context - need to cast away const for compatibility with older FFmpeg
    ctx->decoder_codec = (AVCodec *)decoder_codec;
    
    // Create decoder context
    ctx->decoder_ctx = avcodec_alloc_context3(ctx->decoder_codec);
    if (!ctx->decoder_ctx) {
        fprintf(stderr, "Failed to allocate decoder context\n");
        return -1;
    }
    
    // Copy codec parameters to decoder context
    ret = avcodec_parameters_to_context(ctx->decoder_ctx, ctx->fmt_ctx->streams[ctx->video_stream_idx]->codecpar);
    if (ret < 0) {
        fprintf(stderr, "Failed to copy codec parameters to decoder context\n");
        return -1;
    }
    
    // Open codec
    ret = avcodec_open2(ctx->decoder_ctx, ctx->decoder_codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Failed to open codec\n");
        return -1;
    }
    
    // Allocate frame and packet
    ctx->frame = av_frame_alloc();
    if (!ctx->frame) {
        fprintf(stderr, "Failed to allocate frame\n");
        return -1;
    }
    
    ctx->pkt = av_packet_alloc();
    if (!ctx->pkt) {
        fprintf(stderr, "Failed to allocate packet\n");
        return -1;
    }
    
    // Initialize SwScale context for cropping and scaling
    // Using left eye only (INPUT_WIDTH/2) as source width
    ctx->sws_ctx = sws_getContext(
        INPUT_WIDTH/2, INPUT_HEIGHT,  // Source width/height - use half width for left eye
        AV_PIX_FMT_YUV420P,           // Source format
        OUTPUT_WIDTH, OUTPUT_HEIGHT,  // Destination width/height
        AV_PIX_FMT_YUV420P,           // Destination format
        SWS_BICUBIC,                  // High quality algorithm
        NULL, NULL, NULL              // Default parameters
    );
    
    if (!ctx->sws_ctx) {
        fprintf(stderr, "Failed to initialize SwScale context\n");
        return -1;
    }
    
    // Allocate scaled buffer (output size)
    int scaled_y_size = OUTPUT_WIDTH * OUTPUT_HEIGHT;
    int scaled_uv_size = scaled_y_size / 4;
    ctx->scaled_buffer = (uint8_t*)malloc(scaled_y_size + 2 * scaled_uv_size);
    if (!ctx->scaled_buffer) {
        fprintf(stderr, "Failed to allocate scaled buffer\n");
        return -1;
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    int skip_frames = 0;  // Default: process all frames
    const char *input_file = NULL;
    const char *output_file = NULL;
    
    // Parse command line arguments
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "Usage: %s <input_hevc> <output_file> [skip]\n", argv[0]);
        fprintf(stderr, "       <output_file> can be .hevc for raw HEVC or .mp4 for MP4 container\n");
        fprintf(stderr, "       Add 'skip' to skip every other input frame\n");
        return 1;
    }
    
    input_file = argv[1];
    output_file = argv[2];
    
    if (argc == 4 && strcmp(argv[3], "skip") == 0) {
        skip_frames = 1;
        printf("Frame skipping enabled: processing every other input frame\n");
    }
    
    // Detect output format based on file extension
    int mp4_output = 0;
    const char *ext = strrchr(output_file, '.');
    if (ext && strcmp(ext, ".mp4") == 0) {
        mp4_output = 1;
        printf("Using MP4 container for output\n");
    } else {
        printf("Using raw HEVC for output\n");
    }
    
    ProcessingContext ctx = {0};
    ctx.skip_frames = skip_frames;
    ctx.mp4_output = mp4_output;
    int ret;
    
    // Initialize components
    if (init_decoder(&ctx, input_file) < 0 || init_encoder(&ctx) < 0) {
        fprintf(stderr, "Error: Initialization failed\n");
        cleanup(&ctx);
        return 1;
    }
    
    // Initialize output based on format
    if (mp4_output) {
        if (init_mp4_muxer(&ctx, output_file) < 0) {
            fprintf(stderr, "Error: MP4 muxer initialization failed\n");
            cleanup(&ctx);
            return 1;
        }
    } else {
        // Open raw HEVC output file
        ctx.output_file = fopen(output_file, "wb");
        if (!ctx.output_file) {
            fprintf(stderr, "Error: Could not open output file: %s\n", output_file);
            cleanup(&ctx);
            return 1;
        }
    }
    
    // Process frames
    int frame_count = 0;      // Count of processed frames
    int input_frame_count = 0; // Count of input frames seen
    x265_nal *nals = NULL;
    uint32_t nal_count = 0;
    
    // For timestamp conversion
    AVRational input_time_base;
    AVRational output_time_base = {1, OUTPUT_TIMEBASE}; // Output time base is 1/48000
    
    if (ctx.fmt_ctx && ctx.video_stream_idx >= 0) {
        input_time_base = ctx.fmt_ctx->streams[ctx.video_stream_idx]->time_base;
        printf("Input video time base: %d/%d\n", input_time_base.num, input_time_base.den);
    } else {
        input_time_base.num = 1;
        input_time_base.den = FRAME_RATE;
    }
    
    printf("Output video time base: %d/%d\n", output_time_base.num, output_time_base.den);
    
    // Calculate the timestamp increment for each frame
    // For 50 fps: 48000 / 50 = 960 units per frame
    // For 25 fps (skip mode): 48000 / 25 = 1920 units per frame
    int timestamp_increment = ctx.skip_frames ? 
                             (OUTPUT_TIMEBASE / (FRAME_RATE / 2)) : 
                             (OUTPUT_TIMEBASE / FRAME_RATE);
                             
    printf("Using timestamp increment of %d units per frame\n", timestamp_increment);
    
    printf("Starting to process frames...\n");
    
    // Get the headers from the encoder first (VPS, SPS, PPS)
    ret = x265_encoder_headers(ctx.encoder, &nals, &nal_count);
    if (ret < 0) {
        fprintf(stderr, "Error getting encoder headers\n");
        cleanup(&ctx);
        return 1;
    }
    
    // Write headers based on output format
    if (mp4_output) {
        // Store HEVC headers as extradata for MP4
        if (write_hevc_headers_to_mp4(&ctx, nals, nal_count) < 0) {
            fprintf(stderr, "Failed to write HEVC headers to MP4\n");
            cleanup(&ctx);
            return 1;
        }
    } else {
        // Write headers to raw HEVC output file
        for (uint32_t i = 0; i < nal_count; i++) {
            // Add HEVC start code (0x00 0x00 0x01)
            uint8_t start_code[4] = {0, 0, 0, 1};
            fwrite(start_code, 1, 4, ctx.output_file);
            
            // Write NAL unit
            fwrite(nals[i].payload, 1, nals[i].sizeBytes, ctx.output_file);
        }
    }
    
    // Main processing loop using FFmpeg's demuxing API
    while (av_read_frame(ctx.fmt_ctx, ctx.pkt) >= 0) {
        // Check if this packet belongs to the video stream
        if (ctx.pkt->stream_index == ctx.video_stream_idx) {
            // Save packet timestamp for later use if frame PTS is invalid
            int64_t pkt_pts = ctx.pkt->pts;
            int64_t pkt_dts = ctx.pkt->dts;
            
            // Send packet to decoder
            ret = avcodec_send_packet(ctx.decoder_ctx, ctx.pkt);
            if (ret < 0) {
                fprintf(stderr, "Error sending packet for decoding\n");
                av_packet_unref(ctx.pkt);
                continue;
            }
            
            // Receive decoded frames
            while (ret >= 0) {
                ret = avcodec_receive_frame(ctx.decoder_ctx, ctx.frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    fprintf(stderr, "Error during decoding\n");
                    break;
                }
                
                // Decide whether to process this frame or skip it
                int should_process = 1;
                if (ctx.skip_frames && (input_frame_count % 2 == 1)) {
                    should_process = 0;  // Skip this frame
                }
                
                if (should_process) {
                    // Process frame: crop and scale using SwScale
                    process_frame_with_swscale(&ctx, ctx.frame);
                    
                    // Get timestamp from input frame for informational purposes
                    int64_t input_pts = ctx.frame->pts;
                    if (input_pts == AV_NOPTS_VALUE) {
                        // If no valid PTS in the frame, try packet PTS or DTS
                        if (pkt_pts != AV_NOPTS_VALUE) {
                            input_pts = pkt_pts;
                        } else if (pkt_dts != AV_NOPTS_VALUE) {
                            input_pts = pkt_dts;
                        } else {
                            // Last resort: use frame count
                            input_pts = input_frame_count;
                        }
                    }
                    
                    // Calculate timestamp for output frame based on frame count and timebase
                    int64_t output_pts = frame_count * timestamp_increment;
                    
                    printf("Frame %d: Input PTS = %lld, Output PTS = %lld\n", 
                          input_frame_count, (long long)input_pts, (long long)output_pts);
                    
                    // Prepare for encoding with correct timestamps for timebase 1/48000
                    prepare_for_encoding(&ctx, output_pts);
                    
                    // Force keyframe at the start
                    if (frame_count == 0) {
                        ctx.enc_pic->sliceType = X265_TYPE_IDR;
                    }
                    
                    // Encode the frame
                    ret = x265_encoder_encode(ctx.encoder, &nals, &nal_count, ctx.enc_pic, NULL);
                    if (ret < 0) {
                        fprintf(stderr, "Error encoding frame: %d\n", ret);
                        break;
                    }
                    
                    // Process encoded NALs based on output format
                    if (nal_count > 0) {
                        if (mp4_output) {
                            // Write to MP4 container
                            int is_keyframe = 0;
                            // Check if this is a keyframe by looking for IDR NAL type
                            for (uint32_t i = 0; i < nal_count; i++) {
                                // HEVC NAL unit type is in the first byte, bits 1-6 (7 bits total)
                                uint8_t nal_type = (nals[i].payload[0] >> 1) & 0x3F;
                                // Types 16-21 are IRAP (keyframe) types
                                if (nal_type >= 16 && nal_type <= 21) {
                                    is_keyframe = 1;
                                    break;
                                }
                            }
                            
                            write_nals_to_mp4(&ctx, nals, nal_count, output_pts, is_keyframe);
                        } else {
                            // Write to raw HEVC file
                            for (uint32_t i = 0; i < nal_count; i++) {
                                // Add HEVC start code (0x00 0x00 0x01)
                                uint8_t start_code[4] = {0, 0, 0, 1};
                                fwrite(start_code, 1, 4, ctx.output_file);
                                
                                // Write NAL unit
                                fwrite(nals[i].payload, 1, nals[i].sizeBytes, ctx.output_file);
                            }
                        }
                    }
                    
                    frame_count++;
                    
                    // Print progress
                    if (frame_count % 10 == 0) {
                        printf("Processed %d frames\n", frame_count);
                    }
                } else {
                    printf("Skipping input frame %d\n", input_frame_count);
                }
                
                input_frame_count++;
                
                // Unref the frame
                av_frame_unref(ctx.frame);
            }
        }
        
        // Unref the packet
        av_packet_unref(ctx.pkt);
    }
    
    // Flush encoder
    while (1) {
        ret = x265_encoder_encode(ctx.encoder, &nals, &nal_count, NULL, NULL);
        if (ret <= 0) break;
        
        // Process remaining NALs
        if (mp4_output) {
            // Write to MP4 container - we assume flush packets are not keyframes
            write_nals_to_mp4(&ctx, nals, nal_count, ctx.next_pts + timestamp_increment, 0);
            ctx.next_pts += timestamp_increment;
        } else {
            // Write to raw HEVC file
            for (uint32_t i = 0; i < nal_count; i++) {
                // Add HEVC start code (0x00 0x00 0x01)
                uint8_t start_code[4] = {0, 0, 0, 1};
                fwrite(start_code, 1, 4, ctx.output_file);
                
                // Write NAL unit
                fwrite(nals[i].payload, 1, nals[i].sizeBytes, ctx.output_file);
            }
        }
    }
    
    printf("Done! Processed %d frames out of %d input frames\n", frame_count, input_frame_count);
    
    cleanup(&ctx);
    return 0;
}
