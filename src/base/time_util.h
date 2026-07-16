// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <chrono>
#include <concepts>
#include <ctime>
#include <string>
#include <string_view>

namespace ibmw::common::util {

template <typename Clock>
concept CompatibleClock = requires {
  typename Clock::time_point;
  { Clock::now() } -> std::same_as<typename Clock::time_point>;
};

enum TimeConstant {
  kSecondPerMinute = 60,
  kMinutePerHour = 60,
  kSecondPerHour = 3600,
  kHourPerDay = 24,
  kSecondPerDay = 86400,
  kMsPerSecond = 1000,
  kUsPerMs = 1000,
  kUsPerSecond = 1000000,
  kDayPerMonth = 30,
  kDayPerWeek = 7,
  kMonthPerYear = 12,
};

/**
 * @brief Converts a time point to nanoseconds since epoch
 */
template <typename Clock>
  requires CompatibleClock<Clock>
inline uint64_t GetTimestampNs(std::chrono::time_point<Clock> t) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline uint64_t GetCurTimestampNs() {
  return GetTimestampNs(Clock::now());
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline const std::chrono::time_point<Clock> GetTimePointFromTimestampNs(uint64_t ns) {
  return std::chrono::time_point<Clock>(
      std::chrono::duration_cast<typename std::chrono::time_point<Clock>::duration>(
          std::chrono::nanoseconds(ns)));
}

/**
 * @brief Converts a time point to microseconds since epoch
 */
template <typename Clock>
  requires CompatibleClock<Clock>
inline uint64_t GetTimestampUs(std::chrono::time_point<Clock> t) {
  return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline uint64_t GetCurTimestampUs() {
  return GetTimestampUs(Clock::now());
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline const std::chrono::time_point<Clock> GetTimePointFromTimestampUs(uint64_t us) {
  return std::chrono::time_point<Clock>(
      std::chrono::duration_cast<typename std::chrono::time_point<Clock>::duration>(
          std::chrono::microseconds(us)));
}

/**
 * @brief Converts a time point to milliseconds since epoch
 */
template <typename Clock>
  requires CompatibleClock<Clock>
inline uint64_t GetTimestampMs(std::chrono::time_point<Clock> t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline uint64_t GetCurTimestampMs() {
  return GetTimestampMs(Clock::now());
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline const std::chrono::time_point<Clock> GetTimePointFromTimestampMs(uint64_t ms) {
  return std::chrono::time_point<Clock>(
      std::chrono::duration_cast<typename std::chrono::time_point<Clock>::duration>(
          std::chrono::milliseconds(ms)));
}

/**
 * @brief Converts a time point to seconds since epoch
 */
template <typename Clock>
  requires CompatibleClock<Clock>
inline uint64_t GetTimestampSec(std::chrono::time_point<Clock> t) {
  return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline uint64_t GetCurTimestampSec() {
  return GetTimestampSec(Clock::now());
}

template <typename Clock = std::chrono::system_clock>
  requires CompatibleClock<Clock>
inline const std::chrono::time_point<Clock> GetTimePointFromTimestampSec(uint64_t sec) {
  return std::chrono::time_point<Clock>(
      std::chrono::duration_cast<typename std::chrono::time_point<Clock>::duration>(
          std::chrono::seconds(sec)));
}

inline time_t GetCurTimeT() {
  return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

inline struct tm TimeT2TmLocal(time_t t) {
  struct tm st;
#if defined(_WIN32)
  localtime_s(&st, &t);
#else
  localtime_r(&t, &st);
#endif
  return st;
}

inline struct tm TimeT2TmGm(time_t t) {
  struct tm st;
#if defined(_WIN32)
  gmtime_s(&st, &t);
#else
  gmtime_r(&t, &st);
#endif
  return st;
}

inline std::string_view GetTimeStr(tm t) {
  thread_local char buf[20];  // 4+2+2+1+2+2+2+1
  snprintf(buf,
           sizeof(buf),
           "%04d-%02d-%02d %02d:%02d:%02d",
           (t.tm_year + 1900) % 10000u,
           (t.tm_mon + 1) % 100u,
           (t.tm_mday) % 100u,
           (t.tm_hour) % 100u,
           (t.tm_min) % 100u,
           (t.tm_sec) % 100u);
  return std::string_view(buf);
}

inline std::string_view GetTimeStr(time_t t) {
  return GetTimeStr(TimeT2TmLocal(t));
}

inline struct tm GetCurTm() {
  return TimeT2TmLocal(GetCurTimeT());
}

inline std::string_view GetCurTimeStr() {
  return GetTimeStr(GetCurTimeT());
}

inline bool IsLeapYear(uint32_t year) {
  return (year % 100) ? (year % 4 == 0) : (year % 400 == 0);
}

inline uint32_t GetMonthDayCount(uint32_t year, uint32_t month) {
  static constexpr uint32_t kMonthDayCount[] = {
      31,  // Jan
      28,  // Feb
      31,  // Mar
      30,  // Apr
      31,  // May
      30,  // Jun
      31,  // Jul
      31,  // Aug
      30,  // Sep
      31,  // Oct
      30,  // Nov
      31,  // Dec
  };
  return (month == 1 && IsLeapYear(year)) ? 29 : kMonthDayCount[month];
}

inline int32_t GetLocalTimeZone() {
  time_t now = GetCurTimeT();
  struct tm gt, lt;

#if defined(_WIN32)
  gmtime_s(&gt, &now);
  localtime_s(&lt, &now);
#else
  gmtime_r(&now, &gt);
  localtime_r(&now, &lt);
#endif

  time_t gmt = mktime(&gt);
  time_t lmt = mktime(&lt);

  return (int32_t)difftime(lmt, gmt);
}

inline bool IsPassDay(time_t l_time, time_t r_time, int32_t time_zone) {
  time_t l_day = static_cast<time_t>((l_time + time_zone) / kSecondPerDay);
  time_t r_day = static_cast<time_t>((r_time + time_zone) / kSecondPerDay);
  return (l_day > r_day);
}

inline time_t GetDayStartTime(time_t t) {
  struct tm st = TimeT2TmLocal(t);

  st.tm_sec = 0;
  st.tm_min = 0;
  st.tm_hour = 0;

  return mktime(&st);
}

inline uint32_t GetWeekDay(time_t t) {
  struct tm st = TimeT2TmLocal(t);
  return (st.tm_wday) ? (st.tm_wday) : 7;
}

inline uint32_t GetWeekStartTime(time_t t) {
  return static_cast<uint32_t>(GetDayStartTime(t - (GetWeekDay(t) - 1) * kSecondPerDay));
}

inline int32_t GetDayCount(time_t l_time, time_t r_time, int32_t time_zone) {
  return static_cast<int32_t>((l_time + time_zone - r_time) / kSecondPerDay);
}

inline std::string_view GetWeekDayStr(time_t t) {
  static constexpr std::string_view kWeekDays[] = {
      "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  struct tm st = TimeT2TmLocal(t);
  return kWeekDays[st.tm_wday];
}

inline std::string_view GetWeekDayStrShort(time_t t) {
  static constexpr std::string_view kWeekDaysShort[] = {
      "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  struct tm st = TimeT2TmLocal(t);
  return kWeekDaysShort[st.tm_wday];
}

inline std::string_view GetYearStr(time_t t) {
  thread_local char buf[5];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%04d", (tm.tm_year + 1900) % 10000u);
  return std::string_view(buf);
}

inline std::string_view GetMonthStr(time_t t) {
  thread_local char buf[3];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%02d", (tm.tm_mon + 1) % 100u);
  return std::string_view(buf);
}

inline std::string_view GetDayStr(time_t t) {
  thread_local char buf[3];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%02d", tm.tm_mday % 100u);
  return std::string_view(buf);
}

inline std::string_view GetHourStr(time_t t) {
  thread_local char buf[3];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%02d", tm.tm_hour % 100u);
  return std::string_view(buf);
}

inline std::string_view GetMinuteStr(time_t t) {
  thread_local char buf[3];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%02d", tm.tm_min % 100u);
  return std::string_view(buf);
}

inline std::string_view GetSecondStr(time_t t) {
  thread_local char buf[3];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%02d", tm.tm_sec % 100u);
  return std::string_view(buf);
}

inline std::string_view GetDateStr(time_t t) {
  thread_local char buf[11];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           (tm.tm_year + 1900) % 10000u,
           (tm.tm_mon + 1) % 100u,
           tm.tm_mday % 100u);
  return std::string_view(buf);
}

inline std::string_view GetClockStr(time_t t) {
  thread_local char buf[9];
  auto tm = TimeT2TmLocal(t);
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
           tm.tm_hour % 100u,
           tm.tm_min % 100u,
           tm.tm_sec % 100u);
  return std::string_view(buf);
}

}  // namespace ibmw::common::util
