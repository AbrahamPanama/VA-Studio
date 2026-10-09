/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#ifdef _WIN32
#include <windows.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(VACARDS_OPEN_AUDIT_BUILD)
#define VACARDS_OPEN_AUDIT_API __declspec(dllexport)
#else
#define VACARDS_OPEN_AUDIT_API __declspec(dllimport)
#endif
VACARDS_OPEN_AUDIT_API BOOL WINAPI VacardsOpenAuditStartW(LPCWSTR event_file, LPCWSTR case_id);
VACARDS_OPEN_AUDIT_API BOOL WINAPI VacardsOpenAuditCheckCoverage(void);
VACARDS_OPEN_AUDIT_API void WINAPI VacardsOpenAuditStop(void);
#ifdef __cplusplus
}
#endif
#endif

#ifdef __APPLE__
#ifdef __cplusplus
extern "C" {
#endif
int VacardsOpenAuditStart(const char *event_file, const char *case_id);
int VacardsOpenAuditCheckCoverage(void);
void VacardsOpenAuditStop(void);
#ifdef __cplusplus
}
#endif
#endif
