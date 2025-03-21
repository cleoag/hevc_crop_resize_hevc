# HEVC Crop and Resize Processor

A utility to extract the left eye from 180° stereo fisheye HEVC videos, crop and resize to 720x720, and re-encode with x265.

## Description

This tool processes HEVC (H.265) video files by:
1. Decoding the input HEVC video using FFmpeg/Libav
2. Extracting only the left eye from a 180° stereo fisheye video (cropping from 5760×2880 to 2880×2880)
3. Resizing to 720×720 square output
4. Re-encoding to HEVC with x265

## Prerequisites

To build and run this tool, you'll need:

- GCC or compatible C compiler
- FFmpeg development libraries:
  - libavcodec
  - libavformat
  - libavutil
  - libswscale
- x265 encoder library
- libm (math library)

### Installing Dependencies on Ubuntu/Debian

```bash
sudo apt update
sudo apt install build-essential
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
sudo apt install libx265-dev
```

### Installing Dependencies on Windows

For Windows, you can use MSYS2 or compile using Visual Studio with the appropriate libraries.

## Compilation

Compile the program using GCC:

```bash
gcc -o hevc_processor hevc_processor.c -lavcodec -lavformat -lavutil -lswscale -lx265 -lm
```

## Usage

```bash
./hevc_processor <input_hevc> <output_hevc>
```

Where:
- `<input_hevc>`: Path to the input HEVC file (180° stereo fisheye video, typically 5760×2880)
- `<output_hevc>`: Path where the output HEVC file will be saved (720×720)

### Example

```bash
./hevc_processor input.hevc output.hevc
```

## Processing Details

- Input: 180° stereo fisheye HEVC video (5760×2880)
- Processing:
  - Decodes HEVC frames using FFmpeg/Libav
  - Extracts left eye (cropping to 2880×2880)
  - Scales down to 720×720 using bilinear interpolation
  - Re-encodes using x265 with optimized parameters
- Output: 720×720 HEVC video with left eye only

## Performance

The tool is optimized for lower resource usage with these encoding settings:
- Ultrafast preset
- Zero latency mode
- No B-frames
- Single reference frame
- Single thread processing
- 1 Mbps target bitrate

## Troubleshooting

If you encounter errors related to:

1. **Library not found**: Make sure all required development libraries are installed
2. **Compilation errors**: Check your compiler version and ensure all dependencies are correctly installed
3. **Runtime errors**: Ensure your input file is a valid HEVC file

## License

This software is provided as-is without any warranty. 