// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#ifndef CEPH_COMMON_ACCESSLOG_H
#define CEPH_COMMON_ACCESSLOG_H

namespace ceph::logging {

class AccessLog {
public:
  AccessLog();
  ~AccessLog();

  void log_entry(const Entry & e);
  void set_log_file(std::string_view fn);
  void set_hostname(const std::string& host);
  void reopen_log_file();

private:
  std::string m_log_file;
  std::string m_hostname;
  int m_fd = -1;
  int m_fd_last_error = 0;

};


} // ceph::logging

#endif
