#pragma once

#include <Windows.h>
#include <stdint.h>

typedef LONG NTSTATUS;
#define NT_ERROR(status) ((NTSTATUS)(status) < 0)
inline void DbgPrint(const char*, ...) {}

uint32_t UsbTestNow();
#define GetTickCount UsbTestNow
