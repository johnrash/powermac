/*
 * PowerMac emulator - minimal PNG writer.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "PngWriter.h"

#include <cstdio>
#include <vector>

#include <zlib.h>

namespace powermac {

static u32
Crc32(const u8* data, size_t length, u32 crc = 0xffffffff)
{
	static u32 table[256];
	static bool initialized = false;
	if (!initialized) {
		for (u32 n = 0; n < 256; n++) {
			u32 c = n;
			for (int k = 0; k < 8; k++)
				c = (c & 1) != 0 ? 0xedb88320 ^ (c >> 1) : c >> 1;
			table[n] = c;
		}
		initialized = true;
	}
	for (size_t i = 0; i < length; i++)
		crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	return crc;
}


static void
Put32(std::vector<u8>& out, u32 value)
{
	out.push_back(value >> 24);
	out.push_back(value >> 16);
	out.push_back(value >> 8);
	out.push_back(value);
}


static void
Chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data)
{
	Put32(out, (u32)data.size());
	size_t start = out.size();
	out.insert(out.end(), type, type + 4);
	out.insert(out.end(), data.begin(), data.end());
	u32 crc = Crc32(&out[start], out.size() - start) ^ 0xffffffff;
	Put32(out, crc);
}


bool
WritePng(const std::string& path, const u32* pixels, int width, int height,
	int stride)
{
	// filter "up" on every line (screens compress well with it)
	std::vector<u8> raw;
	raw.reserve((size_t)(width * 3 + 1) * height);
	std::vector<u8> previous((size_t)width * 3, 0);
	std::vector<u8> line((size_t)width * 3);
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			u32 p = pixels[(size_t)y * stride + x];
			line[x * 3] = (u8)(p >> 16);
			line[x * 3 + 1] = (u8)(p >> 8);
			line[x * 3 + 2] = (u8)p;
		}
		raw.push_back(2);
		for (size_t i = 0; i < line.size(); i++)
			raw.push_back((u8)(line[i] - previous[i]));
		previous.swap(line);
	}
	uLongf zLength = compressBound((uLong)raw.size());
	std::vector<u8> z(zLength);
	if (compress2(z.data(), &zLength, raw.data(), (uLong)raw.size(), 6)
			!= Z_OK)
		return false;
	z.resize(zLength);

	std::vector<u8> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
	std::vector<u8> header;
	Put32(header, width);
	Put32(header, height);
	header.push_back(8);	// bit depth
	header.push_back(2);	// truecolor
	header.push_back(0);
	header.push_back(0);
	header.push_back(0);
	Chunk(png, "IHDR", header);
	Chunk(png, "IDAT", z);
	Chunk(png, "IEND", std::vector<u8>());

	FILE* file = fopen(path.c_str(), "wb");
	if (file == nullptr)
		return false;
	bool ok = fwrite(png.data(), 1, png.size(), file) == png.size();
	return fclose(file) == 0 && ok;
}

}	// namespace powermac
