# SPIFFS Assets Builder

This script is used to build the SPIFFS resource partition for an ESP32 project, packaging various resource files into a format that can be used on the device.

## Features

- Process WakeNet models
- Integrate text font files
- Process emoji image collections
- Automatically generate resource index files
- Package and generate the final `assets.bin` file

## Requirements

- Python 3.6+
- Related resource files

## Usage

### Basic Syntax

```bash
./build.py --wakenet_model <wakenet_model_dir> \
    --text_font <text_font_file> \
    --emoji_collection <emoji_collection_dir>
```

### Parameters

| Parameter | Type | Required | Description |
|------|------|------|------|
| `--wakenet_model` | Directory path | No | WakeNet model directory path |
| `--text_font` | File path | No | Text font file path |
| `--emoji_collection` | Directory path | No | Emoji image collection directory path |

### Examples

```bash
# Full parameter example
./build.py \
    --wakenet_model ../../managed_components/espressif__esp-sr/model/wakenet_model/wn9_nihaoxiaozhi_tts \
    --text_font ../../components/xiaozhi-fonts/build/font_puhui_common_20_4.bin \
    --emoji_collection ../../components/xiaozhi-fonts/build/emojis_64/

# Process only font files
./build.py --text_font ../../components/xiaozhi-fonts/build/font_puhui_common_20_4.bin

# Process only emoji files
./build.py --emoji_collection ../../components/xiaozhi-fonts/build/emojis_64/
```

## Workflow

1. **Create build directory structure**
   - `build/` - Main build directory
   - `build/assets/` - Resource file directory
   - `build/output/` - Output file directory

2. **Process WakeNet model**
   - Copy model files to the build directory
   - Use `pack_model.py` to generate `srmodels.bin`
   - Copy the generated model file to the resources directory

3. **Process text fonts**
   - Copy font files to the resources directory
   - Supports `.bin` format font files

4. **Process emoji collection**
   - Scan image files in the specified directory
   - Supports `.png` and `.gif` formats
   - Automatically generate emoji indexes

5. **Generate configuration files**
   - `index.json` - Resource index file
   - `config.json` - Build configuration file

6. **Package final resources**
   - Use `spiffs_assets_gen.py` to generate `assets.bin`
   - Copy to the build root directory

## Output Files

After the build is complete, the following files are generated under the `build/` directory:

- `assets/` - All resource files
- `assets.bin` - Final SPIFFS resource file
- `config.json` - Build configuration
- `output/` - Intermediate output files

## Supported Resource Formats

- **Model files**: `.bin` (processed by `pack_model.py`)
- **Font files**: `.bin`
- **Image files**: `.png`, `.gif`
- **Configuration files**: `.json`

## Error Handling

The script includes comprehensive error handling:

- Check whether source files/directories exist
- Validate subprocess execution results
- Provide detailed error messages and warnings

## Notes

1. Make sure all dependent Python scripts are in the same directory
2. Use absolute paths for resource files, or paths relative to the script directory
3. The build process will clean previous build files
4. The generated `assets.bin` file size is limited by the SPIFFS partition size
