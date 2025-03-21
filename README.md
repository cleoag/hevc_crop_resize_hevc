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
./hevc_processor <input_hevc> <output_hevc> [skip]
```

Where:
- `<input_hevc>`: Path to the input HEVC file (180° stereo fisheye video, typically 5760×2880)
- `<output_hevc>`: Path where the output HEVC file will be saved (720×720)
- `[skip]`: Optional parameter. Add "skip" to process only every other input frame while maintaining the same output frame rate

### Examples

Process all frames:
```bash
./hevc_processor input.hevc output.hevc
```

Process every other frame (for faster processing with same output fps):
```bash
./hevc_processor input.hevc output.hevc skip
```

### Playing Output Files

To play output files at the correct frame rate (50fps), use FFplay:

```bash
ffplay -fflags nobuffer -framedrop -vf "fps=50" output.hevc
```

## Processing Details

- Input: 180° stereo fisheye HEVC video (5760×2880)
- Processing:
  - Decodes HEVC frames using FFmpeg/Libav
  - Extracts left eye (cropping to 2880×2880)
  - Scales down to 720×720 using bilinear interpolation
  - Re-encodes using x265 with optimized parameters
- Output: 720×720 HEVC video with left eye only
- Optional frame skipping for faster processing

## Performance

The tool is optimized for high quality with these encoding settings:
- Medium preset (balanced quality/speed)
- 3 Mbps target bitrate
- Multi-threading with 4 threads
- Frame skipping option for faster processing

## Troubleshooting

If you encounter errors related to:

1. **Library not found**: Make sure all required development libraries are installed
2. **Compilation errors**: Check your compiler version and ensure all dependencies are correctly installed
3. **Runtime errors**: Ensure your input file is a valid HEVC file

## License

This software is provided as-is without any warranty. 