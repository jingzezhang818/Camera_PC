#include "xdmaDLL_public_linux.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr const char* kDevRoot = "/dev";
constexpr const size_t kMaxDeviceSlots = 256;

static bool is_valid_handle(HANDLE handle)
{
	return handle != NULL && handle != INVALID_HANDLE_VALUE;
}

static size_t normalize_alignment(size_t alignment)
{
	if (alignment < sizeof(void*)) {
		alignment = sizeof(void*);
	}
	// posix_memalign requires power-of-two alignment.
	if ((alignment & (alignment - 1)) != 0) {
		size_t p2 = sizeof(void*);
		while (p2 < alignment && p2 <= (SIZE_MAX >> 1)) {
			p2 <<= 1;
		}
		alignment = p2;
	}
	return alignment;
}

static int map_access_to_open_flags(DWORD access)
{
	const bool can_read = (access & GENERIC_READ) != 0;
	const bool can_write = (access & GENERIC_WRITE) != 0;

	if (can_read && can_write) {
		return O_RDWR;
	}
	if (can_write) {
		return O_WRONLY;
	}
	return O_RDONLY;
}

static bool starts_with(const std::string& text, const char* prefix)
{
	const size_t prefix_len = strlen(prefix);
	return text.size() >= prefix_len && text.compare(0, prefix_len, prefix) == 0;
}

static bool ends_with(const std::string& text, const std::string& suffix)
{
	return text.size() >= suffix.size() &&
		text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static std::string normalize_base_path(const char* raw_path)
{
	std::string path = raw_path ? raw_path : "";
	std::replace(path.begin(), path.end(), '\\', '/');
	return path;
}

static bool parse_xdma_index(const char* node_name, int* out_index, std::string* out_suffix)
{
	if (!node_name || !out_index || !out_suffix) {
		return false;
	}

	const std::string name(node_name);
	if (!starts_with(name, "xdma")) {
		return false;
	}

	size_t pos = 4;
	while (pos < name.size() && isdigit(static_cast<unsigned char>(name[pos]))) {
		++pos;
	}
	if (pos == 4 || pos >= name.size() || name[pos] != '_') {
		return false;
	}

	const std::string index_text = name.substr(4, pos - 4);
	char* end_ptr = NULL;
	long index_val = strtol(index_text.c_str(), &end_ptr, 10);
	if (end_ptr == NULL || *end_ptr != '\0' || index_val < 0 || index_val > INT_MAX) {
		return false;
	}

	*out_index = static_cast<int>(index_val);
	*out_suffix = name.substr(pos);
	return true;
}

static std::vector<std::string> enumerate_xdma_base_paths()
{
	std::set<int> dev_indexes;
	DIR* dir = opendir(kDevRoot);
	if (!dir) {
		return {};
	}

	for (;;) {
		dirent* entry = readdir(dir);
		if (!entry) {
			break;
		}

		int dev_index = -1;
		std::string suffix;
		if (!parse_xdma_index(entry->d_name, &dev_index, &suffix)) {
			continue;
		}

		if (suffix == "_user" || starts_with(suffix, "_h2c_") || starts_with(suffix, "_c2h_")
			|| suffix == "_bypass" || starts_with(suffix, "_event_")) {
			dev_indexes.insert(dev_index);
		}
	}

	closedir(dir);

	std::vector<std::string> out;
	out.reserve(dev_indexes.size());
	for (int index : dev_indexes) {
		out.push_back(std::string("/dev/xdma") + std::to_string(index));
	}
	return out;
}

static std::string compose_device_path(const char* base_path, const char* device_name)
{
	const std::string base = normalize_base_path(base_path);
	const std::string channel = device_name ? device_name : "";
	if (base.empty() || channel.empty()) {
		return {};
	}

	const std::string slash_suffix = "/" + channel;
	const std::string underscore_suffix = "_" + channel;
	if (ends_with(base, slash_suffix) || ends_with(base, underscore_suffix)) {
		return base;
	}

	if (!base.empty() && base.back() == '/') {
		return base + channel;
	}

	if (starts_with(base, "/dev/xdma")) {
		return base + "_" + channel;
	}

	return base + "/" + channel;
}

} // namespace

BYTE* allocate_buffer(size_t size, size_t alignment)
{
	if (size == 0) {
		size = 4;
	}

	if (alignment == 0) {
		long page_size = sysconf(_SC_PAGESIZE);
		alignment = page_size > 0 ? static_cast<size_t>(page_size) : 4096u;
	}
	alignment = normalize_alignment(alignment);

	void* ptr = NULL;
	const int rc = posix_memalign(&ptr, alignment, size);
	if (rc != 0) {
		return NULL;
	}
	return static_cast<BYTE*>(ptr);
}

void free_buffer(BYTE* buf)
{
	free(buf);
}

int write_device(HANDLE device, long address, DWORD size, BYTE *buffer)
{
	const int fd = xdma_handle_to_fd(device);
	if (fd < 0) {
		return -3;
	}
	if (lseek(fd, static_cast<off_t>(address), SEEK_SET) == static_cast<off_t>(-1)) {
		return -3;
	}

	const ssize_t wr_size = write(fd, buffer, static_cast<size_t>(size));
	if (wr_size < 0) {
		return -1;
	}
	if (wr_size != static_cast<ssize_t>(size)) {
		return -2;
	}
	return static_cast<int>(size);
}

int read_device(HANDLE device, long address, DWORD size, BYTE *buffer)
{
	const int fd = xdma_handle_to_fd(device);
	if (fd < 0) {
		return -3;
	}
	if (lseek(fd, static_cast<off_t>(address), SEEK_SET) == static_cast<off_t>(-1)) {
		return -3;
	}

	const ssize_t rd_size = read(fd, buffer, static_cast<size_t>(size));
	if (rd_size < 0) {
		return -1;
	}
	if (rd_size != static_cast<ssize_t>(size)) {
		return -2;
	}
	return static_cast<int>(size);
}

int get_devices(GUID guid, char** devpath, size_t len_devpath)
{
	(void)guid;
	std::vector<std::string> bases = enumerate_xdma_base_paths();
	if (!devpath || len_devpath == 0) {
		return static_cast<int>(bases.size());
	}

	// TODO: API does not expose devpath row count. Keep legacy behavior and
	// assume caller pre-allocates enough rows for all returned devices.
	const size_t copy_count = std::min(bases.size(), kMaxDeviceSlots);
	for (size_t i = 0; i < copy_count; ++i) {
		if (!devpath[i]) {
			continue;
		}
		snprintf(devpath[i], len_devpath, "%s", bases[i].c_str());
	}
	return static_cast<int>(bases.size());
}

int open_devices(HANDLE *device_hd, DWORD dwAccessPatter, char *device_base_path, const char *device_name)
{
	if (!device_hd) {
		return 0;
	}
	*device_hd = INVALID_HANDLE_VALUE;

	const std::string device_path = compose_device_path(device_base_path, device_name);
	if (device_path.empty()) {
		return 0;
	}

	const int flags = map_access_to_open_flags(dwAccessPatter) | O_CLOEXEC;
	const int fd = open(device_path.c_str(), flags);
	if (fd < 0) {
		fprintf(stderr, "Error opening device %s, errno=%d\n", device_path.c_str(), errno);
		return 0;
	}

	*device_hd = xdma_fd_to_handle(fd);
	return is_valid_handle(*device_hd) ? 1 : 0;
}

int reset_devices(HANDLE device_hd)
{
	unsigned int val = 1;
	int ret = 0;
	if ((ret = write_device(device_hd, 0x00, 4, (BYTE*)&val)) < 0) {
		return ret;
	}

	val = 0;
	if ((ret = write_device(device_hd, 0x00, 4, (BYTE*)&val)) < 0) {
		return ret;
	}
	return 0;
}

int ready_state(HANDLE device_hd, unsigned int *opstate, unsigned int *DDRstate)
{
	int ret = 0;
	if ((ret = read_device(device_hd, 0x00, 4, (BYTE*)opstate)) < 0) {
		return ret;
	}
	if ((ret = read_device(device_hd, 0x14, 4, (BYTE*)DDRstate)) < 0) {
		return ret;
	}
	return ret;
}

int last_packetEn(HANDLE device_hd)
{
	// TODO: register semantics should be confirmed against current FPGA design.
	unsigned int val = 1;
	int ret = 0;
	if ((ret = write_device(device_hd, 0x1C, 4, (BYTE*)&val)) < 0) {
		return ret;
	}
	return 0;
}

int last_packetSize(HANDLE device_hd)
{
	// TODO: register semantics should be confirmed against current FPGA design.
	unsigned int val = 0;
	int ret = 0;
	if ((ret = read_device(device_hd, 0x20, 4, (BYTE*)&val)) < 0) {
		return ret;
	}
	return static_cast<int>(val);
}

int GXset_channel(HANDLE device_hd, int ch)
{
	// TODO: channel mapping semantics should be confirmed against current FPGA design.
	unsigned int val = 0;
	int ret = 0;
	switch (ch)
	{
	case 1:
		val = 0;
		break;
	case 2:
		val = 1;
		break;
	default:
		val = 0;
		break;
	}
	if ((ret = write_device(device_hd, 0x24, 4, (BYTE*)&val)) < 0) {
		return ret;
	}
	return 0;
}
