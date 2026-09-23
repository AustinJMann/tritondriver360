#include <xtl.h>
#include <xkelib.h>
#include <stdlib.h>

#include "config_storage.h"

namespace ConfigStorage {

namespace {

static const DWORD kMaxConfigBytes = 64 * 1024;
static const DWORD kMaxUsbDevices = 3;
static const char kConfigName[] = "tritonconfig.yml";
static const ACCESS_MASK kSynchronizeAccess = 0x00100000L;
static const DWORD kFileOpen = 1;
static const DWORD kFileCreate = 2;
static const NTSTATUS kStatusSharingViolation = (NTSTATUS)0xC0000043L;

enum ReadResult { kReadMissing, kReadLoaded, kReadInvalid, kReadBusy };

struct NativePaths {
	char usb[kMaxUsbDevices][MAX_PATH];
	char hdd[MAX_PATH];
};

bool BuildConfigPath(const char* volumePath, char output[MAX_PATH]) {
	size_t volumeLength = strlen(volumePath);
	if (!volumeLength || volumeLength + 1 + sizeof(kConfigName) > MAX_PATH)
		return false;
	memcpy(output, volumePath, volumeLength);
	output[volumeLength++] = '\\';
	memcpy(output + volumeLength, kConfigName, sizeof(kConfigName));
	return true;
}

void BuildNativePaths(NativePaths* paths) {
	memset(paths, 0, sizeof(*paths));
	for (DWORD index = 0; index < kMaxUsbDevices; ++index) {
		char volumePath[] = "\\Device\\Mass0";
		volumePath[12] = (char)('0' + index);
		BuildConfigPath(volumePath, paths->usb[index]);
	}
	BuildConfigPath("\\Device\\Harddisk0\\Partition1", paths->hdd);
}

void InitializePathAttributes(const char* path, ANSI_STRING* pathString,
	OBJECT_ATTRIBUTES* attributes) {
	RtlInitAnsiString(pathString, path);
	InitializeObjectAttributes(attributes, pathString, OBJ_CASE_INSENSITIVE, 0);
}

NTSTATUS OpenNativeFile(const char* path, ACCESS_MASK access,
	DWORD shareAccess, DWORD disposition, HANDLE* file, IO_STATUS_BLOCK* ioStatus) {
	ANSI_STRING pathString;
	OBJECT_ATTRIBUTES attributes;
	InitializePathAttributes(path, &pathString, &attributes);
	memset(ioStatus, 0, sizeof(*ioStatus));
	*file = 0;
	DWORD options = FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE;
	if (access & GENERIC_WRITE) options |= FILE_WRITE_THROUGH;
	return NtCreateFile(file, access | kSynchronizeAccess, &attributes, ioStatus,
		0, FILE_ATTRIBUTE_NORMAL, shareAccess, disposition,
		options);
}

void DeleteNativeFile(const char* path) {
	ANSI_STRING pathString;
	OBJECT_ATTRIBUTES attributes;
	InitializePathAttributes(path, &pathString, &attributes);
	NTSTATUS status = NtDeleteFile(&attributes);
	if (!NT_SUCCESS(status))
		DbgPrint("TritonDriver: cannot remove partial config %s status %08X\n",
			path, (unsigned int)status);
}

ReadResult ReadConfig(const char* path, TritonConfig::Config* config) {
	HANDLE file = 0;
	IO_STATUS_BLOCK ioStatus;
	NTSTATUS status = OpenNativeFile(path, GENERIC_READ, FILE_SHARE_READ,
		kFileOpen, &file, &ioStatus);
	if (!NT_SUCCESS(status)) {
		DbgPrint("TritonDriver: config not available at %s status %08X\n",
			path, (unsigned int)status);
		// A file held open for writing exists, so it must not fall through to
		// a lower-priority location.
		return status == kStatusSharingViolation ? kReadBusy : kReadMissing;
	}

	// Read one byte beyond the accepted limit so oversized files are detected
	// without a separate size query.
	char* data = (char*)malloc(kMaxConfigBytes + 2);
	if (!data) {
		DbgPrint("TritonDriver: out of memory reading config %s\n", path);
		NtClose(file);
		return kReadInvalid;
	}
	memset(&ioStatus, 0, sizeof(ioStatus));
	status = NtReadFile(file, 0, 0, 0, &ioStatus, data,
		kMaxConfigBytes + 1, 0);
	NtClose(file);
	if (!NT_SUCCESS(status)) {
		DbgPrint("TritonDriver: failed to read config %s status %08X\n",
			path, (unsigned int)status);
		free(data);
		return kReadInvalid;
	}
	DWORD size = (DWORD)ioStatus.Information;
	if (size > kMaxConfigBytes) {
		DbgPrint("TritonDriver: config %s exceeds %u bytes\n",
			path, (unsigned int)kMaxConfigBytes);
		free(data);
		return kReadInvalid;
	}
	data[size] = 0;
	TritonConfig::ParseError parseError = {};
	bool parsed = TritonConfig::Parse(data, size, config, &parseError);
	free(data);
	if (!parsed) {
		DbgPrint("TritonDriver: invalid config %s line %u: %s; using built-in defaults\n",
			path, (unsigned int)parseError.line,
			parseError.message ? parseError.message : "unknown error");
		return kReadInvalid;
	}
	DbgPrint("TritonDriver: loaded config %s with %u game profile(s)\n",
		path, (unsigned int)config->gameCount);
	return kReadLoaded;
}

bool WriteDefault(const char* path) {
	HANDLE file = 0;
	IO_STATUS_BLOCK ioStatus;
	NTSTATUS status = OpenNativeFile(path, GENERIC_WRITE, 0, kFileCreate,
		&file, &ioStatus);
	if (!NT_SUCCESS(status)) {
		DbgPrint("TritonDriver: cannot create default config %s status %08X\n",
			path, (unsigned int)status);
		return false;
	}

	DWORD size = (DWORD)TritonConfig::DefaultFileSize();
	memset(&ioStatus, 0, sizeof(ioStatus));
	status = NtWriteFile(file, 0, 0, 0, &ioStatus,
		(PVOID)TritonConfig::DefaultFileText(), size, 0);
	DWORD written = (DWORD)ioStatus.Information;
	NtClose(file);
	if (!NT_SUCCESS(status) || written != size) {
		DbgPrint("TritonDriver: failed to finish default config %s status %08X wrote %u/%u\n",
			path, (unsigned int)status, (unsigned int)written, (unsigned int)size);
		DeleteNativeFile(path);
		return false;
	}
	DbgPrint("TritonDriver: created default config %s\n", path);
	return true;
}

// Reads the highest-priority config. An unreadable USB file shadows Hdd1.
// config is written only when the result is kReadLoaded.
ReadResult ReadFirst(const NativePaths& paths, TritonConfig::Config* config) {
	bool unreadableUsbConfig = false;
	for (DWORD i = 0; i < kMaxUsbDevices; ++i) {
		ReadResult result = ReadConfig(paths.usb[i], config);
		if (result == kReadLoaded) return kReadLoaded;
		if (result != kReadMissing) unreadableUsbConfig = true;
	}
	if (unreadableUsbConfig) return kReadInvalid;
	ReadResult result = ReadConfig(paths.hdd, config);
	return result == kReadBusy ? kReadInvalid : result;
}

} // namespace

bool LoadOrCreate(TritonConfig::Config* config) {
	NativePaths paths;
	BuildNativePaths(&paths);

	ReadResult result = ReadFirst(paths, config);
	if (result == kReadLoaded) return true;
	if (result == kReadInvalid) {
		TritonConfig::Initialize(config);
		return false;
	}

	TritonConfig::ParseError error = {};
	if (!TritonConfig::Parse(TritonConfig::DefaultFileText(),
		TritonConfig::DefaultFileSize(), config, &error)) {
		TritonConfig::Initialize(config);
		return false;
	}
	for (DWORD i = 0; i < kMaxUsbDevices; ++i)
		if (WriteDefault(paths.usb[i])) return true;
	if (WriteDefault(paths.hdd)) return true;
	DbgPrint("TritonDriver: no writable config location; using generated defaults in memory\n");
	return true;
}

bool Reload(TritonConfig::Config* config) {
	NativePaths paths;
	BuildNativePaths(&paths);
	if (ReadFirst(paths, config) == kReadLoaded) return true;
	DbgPrint("TritonDriver: config reload failed; keeping current settings\n");
	return false;
}

} // namespace ConfigStorage
