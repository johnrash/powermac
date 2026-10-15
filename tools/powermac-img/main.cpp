/*
 * powermac-img - create, inspect, check and convert PowerMac disk images.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 *   powermac-img create [-f raw|qcow2] [-b BACKING] FILE SIZE
 *   powermac-img info FILE
 *   powermac-img check FILE
 *   powermac-img convert [-O raw|qcow2] SOURCE DESTINATION
 *   powermac-img extract IMAGE PATH OUTPUT   (a file from a BFS volume, e.g.
 *       from a BeOS PowerPC install CD)
 *   powermac-img ls IMAGE [PATH]             (a directory of a BFS volume)
 *   powermac-img attr IMAGE PATH             (the attributes of a file on BFS)
 *
 * Sizes take K, M, G or T suffixes (binary units).
 */
#include "core/storage/BfsReader.h"
#include "core/storage/DiskImage.h"
#include "core/storage/Qcow2Image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

using namespace powermac;

namespace {

void
Usage()
{
	fprintf(stderr,
		"usage: powermac-img create [-f raw|qcow2] [-b BACKING] FILE SIZE\n"
		"       powermac-img info FILE\n"
		"       powermac-img check FILE\n"
		"       powermac-img convert [-O raw|qcow2] SOURCE DESTINATION\n"
		"       powermac-img extract IMAGE PATH OUTPUT\n"
		"       powermac-img ls IMAGE [PATH]\n"
		"       powermac-img attr IMAGE PATH\n"
		"SIZE takes K, M, G or T suffixes; qcow2 is the default format.\n");
	exit(2);
}


bool
ParseSize(const char* text, u64& size)
{
	char* end;
	double value = strtod(text, &end);
	if (end == text || value < 0)
		return false;
	u64 unit = 1;
	switch (*end) {
		case 'k': case 'K': unit = 1ULL << 10; end++; break;
		case 'm': case 'M': unit = 1ULL << 20; end++; break;
		case 'g': case 'G': unit = 1ULL << 30; end++; break;
		case 't': case 'T': unit = 1ULL << 40; end++; break;
	}
	if (*end == 'B' || *end == 'b')
		end++;
	if (*end != '\0')
		return false;
	size = (u64)(value * unit);
	size = (size + 511) / 512 * 512;
	return size > 0;
}


std::string
HumanSize(u64 bytes)
{
	char text[32];
	if (bytes >= (1ULL << 30))
		snprintf(text, sizeof(text), "%.1f GB", bytes / 1073741824.0);
	else if (bytes >= (1ULL << 20))
		snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
	else
		snprintf(text, sizeof(text), "%.1f KB", bytes / 1024.0);
	return text;
}


int
Create(int argc, char** argv)
{
	std::string format = "qcow2";
	std::string backing;
	int i = 0;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
			format = argv[++i];
		else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc)
			backing = argv[++i];
		else
			Usage();
	}
	if (argc - i < 1 || (argc - i < 2 && backing.empty()))
		Usage();
	std::string path = argv[i];
	u64 size = 0;
	std::string error;
	if (argc - i >= 2) {
		if (!ParseSize(argv[i + 1], size)) {
			fprintf(stderr, "bad size '%s'\n", argv[i + 1]);
			return 2;
		}
	} else {
		std::string resolved = backing;
		std::unique_ptr<DiskImage> image = DiskImage::Open(resolved, true,
			error);
		if (!image) {
			fprintf(stderr, "%s\n", error.c_str());
			return 1;
		}
		size = image->Size();
	}
	bool ok;
	if (format == "raw") {
		if (!backing.empty()) {
			fprintf(stderr, "raw images cannot have a backing file\n");
			return 2;
		}
		ok = RawImage::Create(path, size, error);
	} else if (format == "qcow2")
		ok = Qcow2Image::Create(path, size, error, backing);
	else {
		fprintf(stderr, "unknown format '%s'\n", format.c_str());
		return 2;
	}
	if (!ok) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	printf("created %s (%s, %s)\n", path.c_str(), format.c_str(),
		HumanSize(size).c_str());
	return 0;
}


int
Info(const char* path)
{
	std::string error;
	std::unique_ptr<DiskImage> image = DiskImage::Open(path, true, error);
	if (!image) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	struct stat st;
	stat(path, &st);
	printf("image: %s\n", path);
	printf("format: %s\n", image->Format());
	printf("virtual size: %s (%llu bytes)\n", HumanSize(image->Size()).c_str(),
		(unsigned long long)image->Size());
	printf("disk size: %s\n", HumanSize((u64)st.st_blocks * 512).c_str());
	if (Qcow2Image* qcow = dynamic_cast<Qcow2Image*>(image.get())) {
		printf("version: %d\n", qcow->Version());
		printf("cluster size: %u\n", qcow->ClusterSize());
		u64 allocated = qcow->AllocatedClusters();
		printf("allocated: %s (%.1f%%)\n",
			HumanSize(allocated * qcow->ClusterSize()).c_str(),
			100.0 * allocated * qcow->ClusterSize() / image->Size());
		if (!qcow->BackingFile().empty())
			printf("backing file: %s\n", qcow->BackingFile().c_str());
	}
	return 0;
}


int
Check(const char* path)
{
	std::string error;
	std::unique_ptr<Qcow2Image> image = Qcow2Image::Open(path, true, error);
	if (!image) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	std::string report;
	int problems = image->Check(report);
	if (problems == 0) {
		printf("%s: no errors\n", path);
		return 0;
	}
	printf("%s%s: %d problems\n", report.c_str(), path, problems);
	return 3;
}


int
Convert(int argc, char** argv)
{
	std::string format = "qcow2";
	int i = 0;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (strcmp(argv[i], "-O") == 0 && i + 1 < argc)
			format = argv[++i];
		else
			Usage();
	}
	if (argc - i != 2)
		Usage();
	std::string error;
	std::unique_ptr<DiskImage> source = DiskImage::Open(argv[i], true, error);
	if (!source) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	u64 size = source->Size();
	bool ok = format == "raw" ? RawImage::Create(argv[i + 1], size, error)
		: format == "qcow2" ? Qcow2Image::Create(argv[i + 1], size, error)
		: false;
	if (!ok) {
		fprintf(stderr, "%s\n", error.empty() ? "unknown format"
			: error.c_str());
		return 1;
	}
	std::unique_ptr<DiskImage> destination = DiskImage::Open(argv[i + 1],
		false, error);
	if (!destination) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	// copy in 64 KB pieces, leaving out all-zero ones (thin provisioning)
	const u64 kChunk = 65536;
	std::vector<u8> buffer(kChunk);
	for (u64 offset = 0; offset < size; offset += kChunk) {
		size_t length = (size_t)std::min<u64>(kChunk, size - offset);
		if (!source->Read(offset, buffer.data(), length)) {
			fprintf(stderr, "read error at %llu\n",
				(unsigned long long)offset);
			return 1;
		}
		bool zero = true;
		for (size_t j = 0; j < length && zero; j++)
			zero = buffer[j] == 0;
		if (zero)
			continue;
		if (!destination->Write(offset, buffer.data(), length)) {
			fprintf(stderr, "write error at %llu\n",
				(unsigned long long)offset);
			return 1;
		}
	}
	destination->Flush();
	printf("converted %s to %s (%s)\n", argv[i], argv[i + 1], format.c_str());
	return 0;
}


bool
WriteFile(const std::string& path, const std::vector<u8>& data)
{
	FILE* file = fopen(path.c_str(), "wb");
	if (file == nullptr || (!data.empty()
			&& fwrite(data.data(), 1, data.size(), file) != data.size())) {
		fprintf(stderr, "cannot write %s\n", path.c_str());
		if (file != nullptr)
			fclose(file);
		return false;
	}
	fclose(file);
	return true;
}


int
Extract(const char* image, const char* path, const char* output)
{
	std::string error;
	std::vector<u8> data;
	if (!BfsReader::ExtractFromImage(image, path, data, error)) {
		fprintf(stderr, "%s: %s\n", image, error.c_str());
		return 1;
	}
	if (!WriteFile(output, data))
		return 1;
	printf("extracted %s (%zu bytes)\n", path, data.size());
	return 0;
}


int
List(const char* image, const char* path)
{
	std::string error;
	std::vector<BfsReader::Entry> entries;
	if (!BfsReader::ListFromImage(image, path, entries, error)) {
		fprintf(stderr, "%s: %s\n", image, error.c_str());
		return 1;
	}
	for (const BfsReader::Entry& entry : entries) {
		if (entry.directory)
			printf("%12s  %s/\n", "", entry.name.c_str());
		else {
			printf("%12llu  %s\n", (unsigned long long)entry.size,
				entry.name.c_str());
		}
	}
	return 0;
}

int
Attributes(const char* imagePath, const char* path)
{
	std::string error;
	std::unique_ptr<DiskImage> image = DiskImage::Open(imagePath, true,
		error);
	if (!image) {
		fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	BfsReader reader([&image](u64 offset, void* buffer, size_t length) {
		return image->Read(offset, buffer, length);
	});
	std::vector<u64> volumes = reader.FindVolumes(std::vector<u64>());
	u64 block;
	std::vector<BfsAttribute> attributes;
	if (volumes.empty() || !reader.Mount(volumes[0])
		|| !reader.LookupBlock(path, block, error)
		|| !reader.ReadAttributes(block, attributes)) {
		fprintf(stderr, "%s: %s\n", imagePath, error.empty()
			? "no BFS volume" : error.c_str());
		return 1;
	}
	for (const BfsAttribute& attribute : attributes) {
		char type[5] = { (char)(attribute.type >> 24),
			(char)(attribute.type >> 16), (char)(attribute.type >> 8),
			(char)attribute.type, 0 };
		printf("%-32s %s %6zu  ", attribute.name.c_str(), type,
			attribute.data.size());
		bool text = !attribute.data.empty();
		for (size_t i = 0; i + 1 < attribute.data.size(); i++)
			text &= attribute.data[i] >= 32 && attribute.data[i] < 127;
		if (text) {
			printf("\"%.*s\"", (int)attribute.data.size(),
				(const char*)attribute.data.data());
		} else {
			for (size_t i = 0; i < attribute.data.size() && i < 16; i++)
				printf("%02x ", attribute.data[i]);
			if (attribute.data.size() > 16)
				printf("...");
		}
		printf("\n");
	}
	return 0;
}

}	// namespace


int
main(int argc, char** argv)
{
	if (argc < 2)
		Usage();
	std::string command = argv[1];
	if (command == "create")
		return Create(argc - 2, argv + 2);
	if (command == "info" && argc == 3)
		return Info(argv[2]);
	if (command == "check" && argc == 3)
		return Check(argv[2]);
	if (command == "convert")
		return Convert(argc - 2, argv + 2);
	if (command == "extract" && argc == 5)
		return Extract(argv[2], argv[3], argv[4]);
	if (command == "attr" && argc == 4)
		return Attributes(argv[2], argv[3]);
	if (command == "ls" && (argc == 3 || argc == 4))
		return List(argv[2], argc == 4 ? argv[3] : "/");
	Usage();
	return 2;
}
