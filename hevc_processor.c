#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>           // For UCHAR_MAX
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <x265.h>            // x265 encoder

// Configuration parameters
#define INPUT_WIDTH 5760     // Input stereo width
#define INPUT_HEIGHT 2880    // Input height
#define OUTPUT_WIDTH 200     // Output width
#define OUTPUT_HEIGHT 200    // Output height (square)
#define FRAME_RATE 50        // Output frame rate 
#define MAX_NAL_SIZE (4*1024*1024)  // 4MB buffer for NAL units

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
    
    // File I/O
    FILE *output_file;
    
    // Processing options
    int skip_frames;        // 1 to skip every other frame, 0 to process all frames
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
    ctx->encoder_params->fpsNum = FRAME_RATE;
    ctx->encoder_params->fpsDenom = 1;
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
void prepare_for_encoding(ProcessingContext *ctx, int pts) {
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
    
    // Find decoder
    ctx->decoder_codec = avcodec_find_decoder(ctx->fmt_ctx->streams[ctx->video_stream_idx]->codecpar->codec_id);
    if (!ctx->decoder_codec) {
        fprintf(stderr, "Failed to find decoder\n");
        return -1;
    }
    
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
        fprintf(stderr, "Usage: %s <input_hevc> <output_hevc> [skip]\n", argv[0]);
        fprintf(stderr, "       Add 'skip' to skip every other input frame\n");
        return 1;
    }
    
    input_file = argv[1];
    output_file = argv[2];
    
    if (argc == 4 && strcmp(argv[3], "skip") == 0) {
        skip_frames = 1;
        printf("Frame skipping enabled: processing every other input frame\n");
    }
    
    ProcessingContext ctx = {0};
    ctx.skip_frames = skip_frames;
    int ret;
    
    // Open output file
    ctx.output_file = fopen(output_file, "wb");
    if (!ctx.output_file) {
        fprintf(stderr, "Error: Could not open output file: %s\n", output_file);
        return 1;
    }
    
    // Initialize components
    if (init_decoder(&ctx, input_file) < 0 || init_encoder(&ctx) < 0) {
        fprintf(stderr, "Error: Initialization failed\n");
        cleanup(&ctx);
        return 1;
    }
    
    // Process frames
    int frame_count = 0;      // Count of processed frames
    int input_frame_count = 0; // Count of input frames seen
    x265_nal *nals = NULL;
    uint32_t nal_count = 0;
    
    printf("Starting to process frames...\n");
    
    // Get the headers from the encoder first (VPS, SPS, PPS)
    ret = x265_encoder_headers(ctx.encoder, &nals, &nal_count);
    if (ret < 0) {
        fprintf(stderr, "Error getting encoder headers\n");
        cleanup(&ctx);
        return 1;
    }
    
    // Write headers to output file
    for (uint32_t i = 0; i < nal_count; i++) {
        // Add HEVC start code (0x00 0x00 0x01)
        uint8_t start_code[4] = {0, 0, 0, 1};
        fwrite(start_code, 1, 4, ctx.output_file);
        
        // Write NAL unit
        fwrite(nals[i].payload, 1, nals[i].sizeBytes, ctx.output_file);
    }
    
    // Main processing loop using FFmpeg's demuxing API
    while (av_read_frame(ctx.fmt_ctx, ctx.pkt) >= 0) {
        // Check if this packet belongs to the video stream
        if (ctx.pkt->stream_index == ctx.video_stream_idx) {
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
                    
                    // Prepare for encoding
                    prepare_for_encoding(&ctx, frame_count);
                    
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
                    
                    // Write NALs to output file
                    for (uint32_t i = 0; i < nal_count; i++) {
                        // Add HEVC start code (0x00 0x00 0x01)
                        uint8_t start_code[4] = {0, 0, 0, 1};
                        fwrite(start_code, 1, 4, ctx.output_file);
                        
                        // Write NAL unit
                        fwrite(nals[i].payload, 1, nals[i].sizeBytes, ctx.output_file);
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
        
        for (uint32_t i = 0; i < nal_count; i++) {
            // Add HEVC start code (0x00 0x00 0x01)
            uint8_t start_code[4] = {0, 0, 0, 1};
            fwrite(start_code, 1, 4, ctx.output_file);
            
            // Write NAL unit
            fwrite(nals[i].payload, 1, nals[i].sizeBytes, ctx.output_file);
        }
    }
    
    printf("Done! Processed %d frames out of %d input frames\n", frame_count, input_frame_count);
    
    cleanup(&ctx);
    return 0;
}
