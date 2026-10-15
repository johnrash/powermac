/*
 * PowerMac emulator - Macintosh time from a calendar date.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 *
 * Macintosh time counts seconds since 1904-01-01 in local time, so a date
 * converts by the calendar alone, without time zones (and without
 * timegm(), which not every system has).
 */
#ifndef PM_MAC_TIME_H
#define PM_MAC_TIME_H

#include "../Types.h"

namespace powermac {

// Days from 1904-01-01 to the date (proleptic Gregorian; after Howard
// Hinnant's days_from_civil).
inline s64
DaysSince1904(int year, int month, int day)
{
	year -= month <= 2;
	s64 era = (year >= 0 ? year : year - 399) / 400;
	s64 yearOfEra = year - era * 400;
	s64 dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	s64 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100
		+ dayOfYear;
	s64 days1970 = era * 146097 + dayOfEra - 719468;
	return days1970 + 24107;		// 1904-01-01 to 1970-01-01
}


// False if the date is outside what the 32-bit clock holds (1904-2040).
inline bool
MacTimeFromDate(int year, int month, int day, int hour, int minute,
	u32& macTime)
{
	if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0
		|| hour > 23 || minute < 0 || minute > 59)
		return false;
	s64 seconds = DaysSince1904(year, month, day) * 86400 + hour * 3600
		+ minute * 60;
	if (seconds <= 0 || seconds > 0xffffffffLL)
		return false;
	macTime = (u32)seconds;
	return true;
}

}	// namespace powermac

#endif	// PM_MAC_TIME_H
