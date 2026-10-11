/*
 * PS5 port: what ps5sx2_mbedtls_config.h asks mbedTLS's users to supply (2026-10-05, AI-assisted). The PS5's libc
 * exports neither gmtime_r nor explicit_bzero, and gmtime would share one buffer between threads.
 *
 * Copyright (C) 2026 swordpdf
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "mbedtls/build_info.h"
#include "mbedtls/platform_util.h"

#include <stdint.h>
#include <string.h>
#include <time.h>

/* memset through a volatile pointer, so the compiler can't drop it as a dead store (mbedTLS's own fallback). */
static void *(*const volatile ps5sx2_memset)(void *, int, size_t) = memset;

void mbedtls_platform_zeroize(void *buf, size_t len)
{
	if (len > 0)
		ps5sx2_memset(buf, 0, len);
}

/* UTC calendar fields from a time_t: days since 1970-01-01 to a civil date (Howard Hinnant's days_from_civil, run
 * backwards). mbedTLS reads the year, month, day, hour, minute and second; the week and year days are filled too. */
struct tm *mbedtls_platform_gmtime_r(const mbedtls_time_t *tt, struct tm *tm_buf)
{
	if (tt == NULL || tm_buf == NULL)
		return NULL;
	int64_t t = (int64_t)*tt;
	int64_t days = t / 86400, secs = t % 86400;
	if (secs < 0)
	{
		secs += 86400;
		days--;
	}
	const int64_t z = days + 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const int64_t doe = z - era * 146097;
	const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const int64_t mp = (5 * doy + 2) / 153;
	const int64_t d = doy - (153 * mp + 2) / 5 + 1;
	const int64_t m = mp < 10 ? mp + 3 : mp - 9;
	const int64_t y = yoe + era * 400 + (m <= 2);
	if (y - 1900 < INT32_MIN || y - 1900 > INT32_MAX)
		return NULL;
	memset(tm_buf, 0, sizeof(*tm_buf));
	tm_buf->tm_year = (int)(y - 1900);
	tm_buf->tm_mon = (int)(m - 1);
	tm_buf->tm_mday = (int)d;
	tm_buf->tm_hour = (int)(secs / 3600);
	tm_buf->tm_min = (int)(secs / 60 % 60);
	tm_buf->tm_sec = (int)(secs % 60);
	tm_buf->tm_wday = (int)(((days % 7) + 11) % 7); /* 1970-01-01 was a Thursday (4) */
	static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
	const int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
	tm_buf->tm_yday = before[m - 1] + (int)d - 1 + (leap && m > 2 ? 1 : 0);
	return tm_buf;
}
