#include "xdmaDLL_public_linux.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr int kMaxDevices = 16;
constexpr size_t kPathLength = 512;

bool is_valid(HANDLE handle)
{
	return handle != nullptr && handle != INVALID_HANDLE_VALUE;
}

bool env_enabled(const char* key)
{
	const char* value = getenv(key);
	return value && strcmp(value, "1") == 0;
}

} // namespace

int main()
{
	BYTE* tmp = allocate_buffer(4096, 0);
	if (!tmp) {
		fprintf(stderr, "[FAIL] allocate_buffer failed\n");
		return 1;
	}
	free_buffer(tmp);
	printf("[OK] allocate/free buffer\n");

	char paths[kMaxDevices][kPathLength] = {{0}};
	char* ptrs[kMaxDevices] = {0};
	for (int i = 0; i < kMaxDevices; ++i) {
		ptrs[i] = paths[i];
	}

	const int found = get_devices(GUID_DEVINTERFACE_XDMA, ptrs, kPathLength);
	printf("[INFO] get_devices -> %d\n", found);
	if (found <= 0) {
		printf("[SKIP] no /dev/xdma* device found\n");
		return 0;
	}

	HANDLE user = INVALID_HANDLE_VALUE;
	HANDLE h2c0 = INVALID_HANDLE_VALUE;

	if (open_devices(&user, GENERIC_READ | GENERIC_WRITE, ptrs[0], XDMA_FILE_USER) != 1 || !is_valid(user)) {
		fprintf(stderr, "[FAIL] open user channel failed: base=%s\n", ptrs[0]);
		return 1;
	}
	printf("[OK] open user channel: %s_%s\n", ptrs[0], XDMA_FILE_USER);

	if (open_devices(&h2c0, GENERIC_WRITE, ptrs[0], XDMA_FILE_H2C_0) != 1 || !is_valid(h2c0)) {
		fprintf(stderr, "[FAIL] open h2c_0 channel failed: base=%s\n", ptrs[0]);
		CloseHandle(user);
		return 1;
	}
	printf("[OK] open h2c_0 channel: %s_%s\n", ptrs[0], XDMA_FILE_H2C_0);

	unsigned int op = 0;
	unsigned int ddr = 0;
	const int ready_ret = ready_state(user, &op, &ddr);
	if (ready_ret < 0) {
		fprintf(stderr, "[FAIL] ready_state failed: ret=%d\n", ready_ret);
		CloseHandle(h2c0);
		CloseHandle(user);
		return 1;
	}
	printf("[OK] ready_state ret=%d op=%u ddr=%u\n", ready_ret, op, ddr);

	unsigned int reg0 = 0;
	const int read_ret = read_device(user, 0x00, 4, reinterpret_cast<BYTE*>(&reg0));
	if (read_ret < 0) {
		fprintf(stderr, "[FAIL] user BAR read failed at 0x00: ret=%d\n", read_ret);
		CloseHandle(h2c0);
		CloseHandle(user);
		return 1;
	}
	printf("[OK] user BAR read 0x00 -> 0x%08X\n", reg0);

	if (env_enabled("XDMA_SMOKE_ALLOW_USER_WRITE")) {
		const int write_ret = write_device(user, 0x00, 4, reinterpret_cast<BYTE*>(&reg0));
		if (write_ret < 0) {
			fprintf(stderr, "[FAIL] user BAR write failed at 0x00: ret=%d\n", write_ret);
			CloseHandle(h2c0);
			CloseHandle(user);
			return 1;
		}
		printf("[OK] user BAR write-back 0x00 -> 0x%08X\n", reg0);
	} else {
		printf("[SKIP] user BAR write skipped (set XDMA_SMOKE_ALLOW_USER_WRITE=1 to enable)\n");
	}

	if (env_enabled("XDMA_SMOKE_ALLOW_H2C_WRITE")) {
		BYTE payload[256];
		for (size_t i = 0; i < sizeof(payload); ++i) {
			payload[i] = static_cast<BYTE>(i & 0xFF);
		}
		const int h2c_ret = write_device(h2c0, 0x00, static_cast<DWORD>(sizeof(payload)), payload);
		if (h2c_ret < 0) {
			fprintf(stderr, "[FAIL] h2c write failed: ret=%d\n", h2c_ret);
			CloseHandle(h2c0);
			CloseHandle(user);
			return 1;
		}
		printf("[OK] h2c write %d bytes\n", h2c_ret);
	} else {
		printf("[SKIP] h2c write skipped (set XDMA_SMOKE_ALLOW_H2C_WRITE=1 to enable)\n");
	}

	if (env_enabled("XDMA_SMOKE_ALLOW_RESET")) {
		const int reset_ret = reset_devices(user);
		if (reset_ret < 0) {
			fprintf(stderr, "[FAIL] reset_devices failed: ret=%d\n", reset_ret);
			CloseHandle(h2c0);
			CloseHandle(user);
			return 1;
		}
		printf("[OK] reset_devices\n");
	} else {
		printf("[SKIP] reset skipped (set XDMA_SMOKE_ALLOW_RESET=1 to enable)\n");
	}

	if (!CloseHandle(h2c0)) {
		fprintf(stderr, "[FAIL] close h2c_0 failed\n");
		CloseHandle(user);
		return 1;
	}
	if (!CloseHandle(user)) {
		fprintf(stderr, "[FAIL] close user failed\n");
		return 1;
	}
	printf("[OK] close channels\n");

	return 0;
}
