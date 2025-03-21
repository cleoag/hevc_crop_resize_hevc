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
#define OUTPUT_WIDTH 720     // Output width
#define OUTPUT_HEIGHT 720    // Output height (square)
#define FRAME_RATE 60        // Assuming 60fps
#define MAX_NAL_SIZE (4*1024*1024)  // 4MB buffer for NAL units

typedef struct {
    // Libav decoder
    AVCodec *decoder_codec;
    AVCodecContext *decoder_ctx;
    AVFormatContext *fmt_ctx;
    int video_stream_idx;
    AVFrame *frame;
    AVPacket *pkt;
    
    // x265 encoder
    x265_encoder *encoder;
    x265_param *encoder_params;
    x265_picture *enc_pic;
    
    // Processing buffers
    uint8_t *yuv_buffer;
    uint8_t *cropped_buffer;
    uint8_t *scaled_buffer;
    
    // File I/O
    FILE *output_file;
} ProcessingContext;

// Initialize x265 encoder
int init_encoder(ProcessingContext *ctx) {
    // Allocate param structure
    ctx->encoder_params = x265_param_alloc();
    if (!ctx->encoder_params) {
        fprintf(stderr, "Failed to allocate encoder parameters\n");
        return -1;
    }
    
    // Set defaults for preset
    x265_param_default_preset(ctx->encoder_params, "ultrafast", "zerolatency");
    
    // Configure encoder for low resource usage
    ctx->encoder_params->sourceWidth = OUTPUT_WIDTH;
    ctx->encoder_params->sourceHeight = OUTPUT_HEIGHT;
    ctx->encoder_params->fpsNum = FRAME_RATE;
    ctx->encoder_params->fpsDenom = 1;
    ctx->encoder_params->internalCsp = X265_CSP_I420;
    
    // Set low CPU/memory options
    ctx->encoder_params->bframes = 0;                // No B-frames for lower latency
    ctx->encoder_params->maxNumReferences = 1;       // Single reference frame
    ctx->encoder_params->lookaheadDepth = 0;         // No lookahead
    ctx->encoder_params->bEnableRectInter = 0;       // Disable advanced features
    ctx->encoder_params->bEnableAMP = 0;
    ctx->encoder_params->bEnableFastIntra = 1;       // Fast intra decisions
    ctx->encoder_params->bEnableEarlySkip = 1;       // Early skip detection
    ctx->encoder_params->searchMethod = X265_DIA_SEARCH; // Simple diamond search
    ctx->encoder_params->maxCUSize = 32;             // Smaller CU size
    ctx->encoder_params->minCUSize = 8;
    ctx->encoder_params->frameNumThreads = 1;        // Single thread
    ctx->encoder_params->rc.bitrate = 1000;          // 1 Mbps
    ctx->encoder_params->rc.qpMin = 20;              // Lower quality for less processing
    ctx->encoder_params->rc.qpMax = 40;
    ctx->encoder_params->rc.rateControlMode = X265_RC_ABR; // Average bitrate mode
    
    // Set HEVC profile and level for compatibility
    ctx->encoder_params->bRepeatHeaders = 1;         // Include headers with each keyframe
    ctx->encoder_params->bEmitHRDSEI = 1;            // Emit HRD info for better compatibility
    ctx->encoder_params->keyframeMin = 1;            // Minimum GOP size
    ctx->encoder_params->keyframeMax = 120;          // Maximum GOP size
    ctx->encoder_params->bOpenGOP = 0;               // Closed GOP structure
    ctx->encoder_params->levelIdc = 0;               // Let x265 choose level
    
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

// Extract the left eye from a stereo frame
void crop_left_eye(ProcessingContext *ctx) {
    // Y plane
    int half_width = INPUT_WIDTH / 2;
    uint8_t *y_src = ctx->yuv_buffer;
    uint8_t *y_dst = ctx->cropped_buffer;
    
    for (int y = 0; y < INPUT_HEIGHT; y++) {
        memcpy(y_dst, y_src, half_width);
        y_src += INPUT_WIDTH;
        y_dst += half_width;
    }
    
    // U plane
    half_width /= 2;
    uint8_t *u_src = ctx->yuv_buffer + (INPUT_WIDTH * INPUT_HEIGHT);
    uint8_t *u_dst = ctx->cropped_buffer + ((INPUT_WIDTH / 2) * INPUT_HEIGHT);
    
    for (int y = 0; y < INPUT_HEIGHT / 2; y++) {
        memcpy(u_dst, u_src, half_width);
        u_src += INPUT_WIDTH / 2;
        u_dst += half_width;
    }
    
    // V plane
    uint8_t *v_src = ctx->yuv_buffer + (INPUT_WIDTH * INPUT_HEIGHT) + (INPUT_WIDTH * INPUT_HEIGHT / 4);
    uint8_t *v_dst = ctx->cropped_buffer + ((INPUT_WIDTH / 2) * INPUT_HEIGHT) + ((INPUT_WIDTH / 2) * INPUT_HEIGHT / 4);
    
    for (int y = 0; y < INPUT_HEIGHT / 2; y++) {
        memcpy(v_dst, v_src, half_width);
        v_src += INPUT_WIDTH / 2;
        v_dst += half_width;
    }
}

// Simple bilinear scaling from source to destination buffer
void scale_frame(ProcessingContext *ctx) {
    // Scale Y plane
    int src_width = INPUT_WIDTH / 2;
    int src_height = INPUT_HEIGHT;
    uint8_t *y_src = ctx->cropped_buffer;
    uint8_t *y_dst = ctx->scaled_buffer;
    
    for (int y = 0; y < OUTPUT_HEIGHT; y++) {
        float src_y = y * (float)src_height / OUTPUT_HEIGHT;
        int src_y_int = (int)src_y;
        float src_y_frac = src_y - src_y_int;
        
        for (int x = 0; x < OUTPUT_WIDTH; x++) {
            float src_x = x * (float)src_width / OUTPUT_WIDTH;
            int src_x_int = (int)src_x;
            float src_x_frac = src_x - src_x_int;
            
            // Calculate bilinear interpolation
            int index = src_y_int * src_width + src_x_int;
            
            // Handle boundary cases
            if (src_x_int >= src_width - 1) src_x_int = src_width - 2;
            if (src_y_int >= src_height - 1) src_y_int = src_height - 2;
            
            uint8_t p00 = y_src[src_y_int * src_width + src_x_int];
            uint8_t p10 = y_src[src_y_int * src_width + src_x_int + 1];
            uint8_t p01 = y_src[(src_y_int + 1) * src_width + src_x_int];
            uint8_t p11 = y_src[(src_y_int + 1) * src_width + src_x_int + 1];
            
            // Linear interpolation
            float p0 = p00 * (1 - src_x_frac) + p10 * src_x_frac;
            float p1 = p01 * (1 - src_x_frac) + p11 * src_x_frac;
            uint8_t p = (uint8_t)(p0 * (1 - src_y_frac) + p1 * src_y_frac);
            
            y_dst[y * OUTPUT_WIDTH + x] = p;
        }
    }
    
    // Scale U plane
    src_width = INPUT_WIDTH / 4;
    src_height = INPUT_HEIGHT / 2;
    uint8_t *u_src = ctx->cropped_buffer + ((INPUT_WIDTH / 2) * INPUT_HEIGHT);
    uint8_t *u_dst = ctx->scaled_buffer + (OUTPUT_WIDTH * OUTPUT_HEIGHT);
    
    for (int y = 0; y < OUTPUT_HEIGHT / 2; y++) {
        float src_y = y * (float)src_height / (OUTPUT_HEIGHT / 2);
        int src_y_int = (int)src_y;
        float src_y_frac = src_y - src_y_int;
        
        for (int x = 0; x < OUTPUT_WIDTH / 2; x++) {
            float src_x = x * (float)src_width / (OUTPUT_WIDTH / 2);
            int src_x_int = (int)src_x;
            float src_x_frac = src_x - src_x_int;
            
            // Handle boundary cases
            if (src_x_int >= src_width - 1) src_x_int = src_width - 2;
            if (src_y_int >= src_height - 1) src_y_int = src_height - 2;
            
            uint8_t p00 = u_src[src_y_int * src_width + src_x_int];
            uint8_t p10 = u_src[src_y_int * src_width + src_x_int + 1];
            uint8_t p01 = u_src[(src_y_int + 1) * src_width + src_x_int];
            uint8_t p11 = u_src[(src_y_int + 1) * src_width + src_x_int + 1];
            
            float p0 = p00 * (1 - src_x_frac) + p10 * src_x_frac;
            float p1 = p01 * (1 - src_x_frac) + p11 * src_x_frac;
            uint8_t p = (uint8_t)(p0 * (1 - src_y_frac) + p1 * src_y_frac);
            
            u_dst[y * (OUTPUT_WIDTH / 2) + x] = p;
        }
    }
    
    // Scale V plane
    uint8_t *v_src = ctx->cropped_buffer + ((INPUT_WIDTH / 2) * INPUT_HEIGHT) + ((INPUT_WIDTH / 2) * INPUT_HEIGHT / 4);
    uint8_t *v_dst = ctx->scaled_buffer + (OUTPUT_WIDTH * OUTPUT_HEIGHT) + (OUTPUT_WIDTH * OUTPUT_HEIGHT / 4);
    
    for (int y = 0; y < OUTPUT_HEIGHT / 2; y++) {
        float src_y = y * (float)src_height / (OUTPUT_HEIGHT / 2);
        int src_y_int = (int)src_y;
        float src_y_frac = src_y - src_y_int;
        
        for (int x = 0; x < OUTPUT_WIDTH / 2; x++) {
            float src_x = x * (float)src_width / (OUTPUT_WIDTH / 2);
            int src_x_int = (int)src_x;
            float src_x_frac = src_x - src_x_int;
            
            // Handle boundary cases
            if (src_x_int >= src_width - 1) src_x_int = src_width - 2;
            if (src_y_int >= src_height - 1) src_y_int = src_height - 2;
            
            uint8_t p00 = v_src[src_y_int * src_width + src_x_int];
            uint8_t p10 = v_src[src_y_int * src_width + src_x_int + 1];
            uint8_t p01 = v_src[(src_y_int + 1) * src_width + src_x_int];
            uint8_t p11 = v_src[(src_y_int + 1) * src_width + src_x_int + 1];
            
            float p0 = p00 * (1 - src_x_frac) + p10 * src_x_frac;
            float p1 = p01 * (1 - src_x_frac) + p11 * src_x_frac;
            uint8_t p = (uint8_t)(p0 * (1 - src_y_frac) + p1 * src_y_frac);
            
            v_dst[y * (OUTPUT_WIDTH / 2) + x] = p;
        }
    }
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
    
    // Free buffers
    free(ctx->yuv_buffer);
    free(ctx->cropped_buffer);
    free(ctx->scaled_buffer);
    
    // Close files
    if (ctx->output_file) {
        fclose(ctx->output_file);
    }
}

// Convert AVFrame to YUV buffer
void avframe_to_yuv_buffer(ProcessingContext *ctx, AVFrame *frame) {
    // Y plane
    for (int i = 0; i < INPUT_HEIGHT; i++) {
        memcpy(ctx->yuv_buffer + i * INPUT_WIDTH,
               frame->data[0] + i * frame->linesize[0],
               INPUT_WIDTH);
    }
    
    // U plane
    int offset = INPUT_WIDTH * INPUT_HEIGHT;
    for (int i = 0; i < INPUT_HEIGHT / 2; i++) {
        memcpy(ctx->yuv_buffer + offset + i * (INPUT_WIDTH / 2),
               frame->data[1] + i * frame->linesize[1],
               INPUT_WIDTH / 2);
    }
    
    // V plane
    offset += (INPUT_WIDTH * INPUT_HEIGHT) / 4;
    for (int i = 0; i < INPUT_HEIGHT / 2; i++) {
        memcpy(ctx->yuv_buffer + offset + i * (INPUT_WIDTH / 2),
               frame->data[2] + i * frame->linesize[2],
               INPUT_WIDTH / 2);
    }
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
    
    // Allocate buffers
    // Calculate YUV buffer size
    int y_size = INPUT_WIDTH * INPUT_HEIGHT;
    int uv_size = y_size / 4;
    ctx->yuv_buffer = (uint8_t*)malloc(y_size + 2 * uv_size);
    if (!ctx->yuv_buffer) {
        fprintf(stderr, "Failed to allocate YUV buffer\n");
        return -1;
    }
    
    // Allocate processing buffers
    // Left eye buffer (half width)
    int cropped_y_size = (INPUT_WIDTH / 2) * INPUT_HEIGHT;
    int cropped_uv_size = cropped_y_size / 4;
    ctx->cropped_buffer = (uint8_t*)malloc(cropped_y_size + 2 * cropped_uv_size);
    if (!ctx->cropped_buffer) {
        fprintf(stderr, "Failed to allocate cropped buffer\n");
        return -1;
    }
    
    // Scaled buffer (720p)
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
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input_hevc> <output_hevc>\n", argv[0]);
        return 1;
    }
    
    const char *input_file = argv[1];
    const char *output_file = argv[2];
    
    ProcessingContext ctx = {0};
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
    int frame_count = 0;
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
                
                // Copy AVFrame data to our YUV buffer
                avframe_to_yuv_buffer(&ctx, ctx.frame);
                
                // Process the frame
                crop_left_eye(&ctx);
                scale_frame(&ctx);
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
    
    printf("Done! Processed %d frames\n", frame_count);
    
    cleanup(&ctx);
    return 0;
}
