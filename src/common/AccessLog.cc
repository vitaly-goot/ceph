
// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <fcntl.h>
#include <string>
#include <fmt/format.h>

#include "include/compat.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "log/Entry.h"
#include "AccessLog.h"

#define dout_subsys ceph_subsys_alog

namespace ceph::logging {

AccessLog::AccessLog() 
{
}

AccessLog::~AccessLog()
{
  if (m_fd >= 0) close(m_fd);
}

void AccessLog::log_entry(const Entry & e) 
{
  if (m_fd < 0)
    return;
  fmt::memory_buffer buf;
  double unix_ts = e.m_stamp.time_since_epoch().count().count / 1000000000.0;
  time_t timestamp_sec = static_cast<std::time_t>(unix_ts);
  std::tm* gmtime = std::gmtime(&timestamp_sec);
  fmt::format_to(std::back_inserter(buf), R"(time={:0>4}-{:0>2}-{:0>2}T{:0>2}:{:0>2}:{:0>2}Z timestamp={:.3f} {})", 
    gmtime->tm_year + 1900,  // Year (
    gmtime->tm_mon + 1,      // Month (0-based in tm struct)
    gmtime->tm_mday,        
    gmtime->tm_hour,         
    gmtime->tm_min,        
    gmtime->tm_sec,
    unix_ts,
    e.strv()
  );
  fmt::format_to(std::back_inserter(buf), "\n");
  //auto t = ceph::logging::log_clock::to_timeval(e.m_stamp);
  int r = safe_write(m_fd, buf.data(), buf.size());
  if (r != m_fd_last_error) {
    if (r < 0)
      std::cerr << "AccessLog problem writing to " << m_log_file
           << ": " << cpp_strerror(r)
           << std::endl;
    m_fd_last_error = r;
  }
}

void AccessLog::set_log_file(std::string_view fn) 
{
  m_log_file = fn;
}

void AccessLog::set_hostname(const std::string& host)
{
  m_hostname = host;
}

void AccessLog::reopen_log_file()
{
  if (m_fd >= 0) {
    VOID_TEMP_FAILURE_RETRY(::close(m_fd));
    m_fd = -1;
  }
  if (m_log_file.length()) {
    m_fd = ::open(m_log_file.c_str(), O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC, 0644);
  }
}

}
