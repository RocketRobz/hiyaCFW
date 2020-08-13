#include "gif.hpp"
#include "lzw.hpp"
#include "tonccpy.h"

#include <stdio.h>

std::vector<Gif *> Gif::_animating;

void Gif::timerHandler(void) {
	for (auto gif : _animating) {
		gif->displayFrame();
	}
}

void Gif::displayFrame(void) {
	if (_paused || ++_currentDelayProgress <= _currentDelay)
		return;

	_currentDelayProgress = 0;

	Frame &frame = _frames[_currentFrame++];

	if (_currentFrame >= _frames.size()) {
		_currentFrame = 0;
		_currentLoop++;
	}

	if (_currentLoop > _loopCount) {
		_finished = true;
		_paused = true;
		_currentLoop = 0;
		return;
	}

	if (frame.hasGCE) {
		_currentDelay = frame.gce.delay;
		if (frame.gce.delay == 0) {
			_finished = true;
			_paused = true;
		} else if (frame.gce.userInputFlag) {
			_waitingForInput = true;
			_paused = true;
		}
	}

	std::vector<u16> &colorTable = frame.descriptor.lctFlag ? frame.lct : _gct;

	tonccpy(_top ? BG_PALETTE : BG_PALETTE_SUB, colorTable.data(), colorTable.size() * 2);
	tonccpy(_top ? BG_PALETTE : BG_PALETTE_SUB, colorTable.data(), colorTable.size() * 2);

	// Disposal method 2 = fill with bg color
	if (frame.gce.disposalMethod == 2)
		toncset(_top ? BG_GFX : BG_GFX_SUB, header.bgColor, 256 * 192);

	int x = 0, y = 0;
	u8 *dst = (u8*)(_top ? BG_GFX : BG_GFX_SUB) + (frame.descriptor.y + y + (192 - header.height) / 2) * 256 + frame.descriptor.x + (256 - header.width) / 2;
	auto flush_fn = [&dst, &x, &y, &frame](std::vector<u8>::const_iterator begin, std::vector<u8>::const_iterator end) {
		for (; begin != end; ++begin) {
			if (!frame.gce.transparentColorFlag || *begin != frame.gce.transparentColor)
				*(dst + x) = *begin;
			x++;
			if (x >= frame.descriptor.w) {
				y++;
				x = 0;
				dst += 256;
			}
		}
	};

	LZWReader reader(frame.image.lzwMinimumCodeSize, flush_fn);
	reader.decode(frame.image.imageData.begin(), frame.image.imageData.end());
}

bool Gif::load(bool top) {
	_top = top;

	FILE *file = fopen((_top ? "sd:/hiya/splashtop.gif" : "sd:/hiya/splashbottom.gif"), "rb");
	if (!file)
		return false;

	// Reserve space for 2,000 frames
	_frames.reserve(2000);

	// Read header
	fread(&header, 1, sizeof(header), file);

	// Check that this is a GIF
	if (memcmp(header.signature, "GIF89a", sizeof(header.signature)) != 0) {
		fclose(file);
		return false;
	}

	// Load global color table
	if (header.gctFlag) {
		int numColors = (2 << header.gctSize);

		_gct = std::vector<u16>(numColors);
		for (int i = 0; i < numColors; i++) {
			_gct[i] = fgetc(file) >> 3 | (fgetc(file) >> 3) << 5 | (fgetc(file) >> 3) << 10 | BIT(15);
		}
	}

	Frame frame;
	while (1) {
		switch (fgetc(file)) {
			case 0x21: { // Extension
				switch (fgetc(file)) {
					case 0xF9: { // Graphics Control
						frame.hasGCE = true;
						fread(&frame.gce, 1, fgetc(file), file);
						fgetc(file); // Terminator
						break;
					} case 0x01: { // Plain text
						// Unsupported for now, I can't even find a text GIF to test with
						// frame.hasText = true;
						// fread(&frame.textDescriptor, 1, sizeof(frame.textDescriptor), file);
						fseek(file, 12, SEEK_CUR);
						while (u8 size = fgetc(file)) {
							// char temp[size + 1];
							// fread(temp, 1, size, file);
							// frame.text += temp;
							fseek(file, size, SEEK_CUR);
						}
						// _frames.push_back(frame);
						// frame = Frame();
						break;
					} case 0xFF: { // Application extension
						if (fgetc(file) == 0xB) {
							char buffer[0xC] = {0};
							fread(buffer, 1, 0xB, file);
							if (strcmp(buffer, "NETSCAPE2.0") == 0) { // Check for Netscape loop count
								// ptr += 0xB + 2;
								fseek(file, 2, SEEK_CUR);
								fread(&_loopCount, 1, sizeof(_loopCount), file);
								fgetc(file); //terminator
								break;
							}
						}
					} case 0xFE: { // Comment
						// Skip comments and unsupported application extionsions
						while (u8 size = fgetc(file)) {
							fseek(file, size, SEEK_CUR);
						}
						break;
					}
				}
				break;
			} case 0x2C: { // Image desriptor
				frame.hasImage = true;
				fread(&frame.descriptor, 1, sizeof(frame.descriptor), file);
				if (frame.descriptor.lctFlag) {
					int numColors = 2 << frame.descriptor.lctSize;
					frame.lct = std::vector<u16>(numColors);
					for (int i = 0; i < numColors; i++) {
						frame.lct[i] = fgetc(file) >> 3 | (fgetc(file) >> 3) << 5 | (fgetc(file) >> 3) << 10 | BIT(15);
					}
				}

				frame.image.lzwMinimumCodeSize = fgetc(file);
				while (u8 size = fgetc(file)) {
					size_t end = frame.image.imageData.size();
					frame.image.imageData.resize(end + size);
					fread(frame.image.imageData.data() + end, 1, size, file);
				}

				_frames.push_back(frame);
				frame = Frame();
				break;
			} case 0x3B: { // Trailer
				goto breakWhile;
			}
		}
	}
	breakWhile:

	fclose(file);

	_paused = false;
	_finished = loopForever();
	_frames.shrink_to_fit();
	_animating.push_back(this);

	return true;
}