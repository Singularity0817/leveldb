// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// Logger implementation that can be shared by all environments
// where enough posix functionality is available.

#ifndef STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_
#define STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_

#include <sys/time.h>

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <thread>

#include "leveldb/env.h"

#include "libpmem.h"

namespace leveldb {

class PmdkLogger final : public Logger {
 public:
  // Creates a logger that writes to the given file.
  //
  // The PmdkLogger instance takes ownership of the file handle.
  explicit PmdkLogger(const std::string& filename, char *mmap_base, size_t length, bool existing) 
                     : filename_(filename), 
                       mmap_base_(mmap_base), 
                       length_(length),
                       now_off_(existing ?  length : 0) { assert(mmap_base_ != nullptr); }

  ~PmdkLogger() override {
    if (mmap_base_ != NULL) {
      pmem_unmap(mmap_base_, length_);
      size_t new_size;
      mmap_base_ = reinterpret_cast<char *>(pmem_map_file(filename_.c_str(),
                                                          now_off_+1,
                                                          PMEM_FILE_CREATE,
                                                          0666, &new_size, NULL));
      length_ = new_size;
      pmem_unmap(mmap_base_, length_);
    }
  }

  void Logv(const char* format, std::va_list arguments) override {
    // Record the time as close to the Logv() call as possible.
    struct ::timeval now_timeval;
    ::gettimeofday(&now_timeval, nullptr);
    const std::time_t now_seconds = now_timeval.tv_sec;
    struct std::tm now_components;
    ::localtime_r(&now_seconds, &now_components);

    // Record the thread ID.
    constexpr const int kMaxThreadIdSize = 32;
    std::ostringstream thread_stream;
    thread_stream << std::this_thread::get_id();
    std::string thread_id = thread_stream.str();
    if (thread_id.size() > kMaxThreadIdSize) {
      thread_id.resize(kMaxThreadIdSize);
    }

    // We first attempt to print into a stack-allocated buffer. If this attempt
    // fails, we make a second attempt with a dynamically allocated buffer.
    constexpr const int kStackBufferSize = 512;
    char stack_buffer[kStackBufferSize];
    static_assert(sizeof(stack_buffer) == static_cast<size_t>(kStackBufferSize),
                  "sizeof(char) is expected to be 1 in C++");

    int dynamic_buffer_size = 0;  // Computed in the first iteration.
    for (int iteration = 0; iteration < 2; ++iteration) {
      const int buffer_size =
          (iteration == 0) ? kStackBufferSize : dynamic_buffer_size;
      char* const buffer =
          (iteration == 0) ? stack_buffer : new char[dynamic_buffer_size];

      // Print the header into the buffer.
      int buffer_offset = std::snprintf(
          buffer, buffer_size, "%04d/%02d/%02d-%02d:%02d:%02d.%06d %s ",
          now_components.tm_year + 1900, now_components.tm_mon + 1,
          now_components.tm_mday, now_components.tm_hour, now_components.tm_min,
          now_components.tm_sec, static_cast<int>(now_timeval.tv_usec),
          thread_id.c_str());

      // The header can be at most 28 characters (10 date + 15 time +
      // 3 delimiters) plus the thread ID, which should fit comfortably into the
      // static buffer.
      assert(buffer_offset <= 28 + kMaxThreadIdSize);
      static_assert(28 + kMaxThreadIdSize < kStackBufferSize,
                    "stack-allocated buffer may not fit the message header");
      assert(buffer_offset < buffer_size);

      // Print the message into the buffer.
      std::va_list arguments_copy;
      va_copy(arguments_copy, arguments);
      buffer_offset +=
          std::vsnprintf(buffer + buffer_offset, buffer_size - buffer_offset,
                         format, arguments_copy);
      va_end(arguments_copy);

      // The code below may append a newline at the end of the buffer, which
      // requires an extra character.
      if (buffer_offset >= buffer_size - 1) {
        // The message did not fit into the buffer.
        if (iteration == 0) {
          // Re-run the loop and use a dynamically-allocated buffer. The buffer
          // will be large enough for the log message, an extra newline and a
          // null terminator.
          dynamic_buffer_size = buffer_offset + 2;
          continue;
        }

        // The dynamically-allocated buffer was incorrectly sized. This should
        // not happen, assuming a correct implementation of std::(v)snprintf.
        // Fail in tests, recover by truncating the log message in production.
        assert(false);
        buffer_offset = buffer_size - 1;
      }

      // Add a newline if necessary.
      if (buffer[buffer_offset - 1] != '\n') {
        buffer[buffer_offset] = '\n';
        ++buffer_offset;
      }

      assert(buffer_offset <= buffer_size);
      /*
      std::fwrite(buffer, 1, buffer_offset, fp_);
      std::fflush(fp_);
      */
      while (now_off_ + buffer_offset > length_) {
        pmem_unmap(mmap_base_, length_);
        size_t new_size;
        mmap_base_ = reinterpret_cast<char *>(pmem_map_file(filename_.c_str(),
                                                            length_+1024*1024,
                                                            PMEM_FILE_CREATE | PMEM_FILE_SPARSE,
                                                            0666, &new_size, NULL));
        length_ = new_size;
      }
      pmem_memcpy(mmap_base_+now_off_, buffer, buffer_offset, PMEM_F_MEM_NONTEMPORAL);
      now_off_ += buffer_offset;
      if (iteration != 0) {
        delete[] buffer;
      }
      break;
    }
  }

 private:
  //std::FILE* const fp_;
  std::string filename_;
  size_t length_;
  char *mmap_base_;
  size_t now_off_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_POSIX_LOGGER_H_
