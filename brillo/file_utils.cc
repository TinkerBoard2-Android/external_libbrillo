// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "brillo/file_utils.h"

#include <fcntl.h>
#include <unistd.h>

#include <base/files/file_path.h>
#include <base/files/file_util.h>
#include <base/files/scoped_file.h>
#include <base/logging.h>
#include <base/posix/eintr_wrapper.h>
#include <base/rand_util.h>
#include <base/strings/string_number_conversions.h>
#include <base/time/time.h>

namespace brillo {

namespace {

// Log sync(), fsync(), etc. calls that take this many seconds or longer.
constexpr const base::TimeDelta kLongSync = base::TimeDelta::FromSeconds(10);

enum {
  kPermissions600 = S_IRUSR | S_IWUSR,
  kPermissions777 = S_IRWXU | S_IRWXG | S_IRWXO
};

// Verify that base file permission enums are compatible with S_Ixxx. If these
// asserts ever fail, we'll need to ensure that users of these functions switch
// away from using base permission enums and add a note to the function comments
// indicating that base enums can not be used.
static_assert(base::FILE_PERMISSION_READ_BY_USER == S_IRUSR,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_WRITE_BY_USER == S_IWUSR,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_EXECUTE_BY_USER == S_IXUSR,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_READ_BY_GROUP == S_IRGRP,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_WRITE_BY_GROUP == S_IWGRP,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_EXECUTE_BY_GROUP == S_IXGRP,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_READ_BY_OTHERS == S_IROTH,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_WRITE_BY_OTHERS == S_IWOTH,
              "base file permissions don't match unistd.h permissions");
static_assert(base::FILE_PERMISSION_EXECUTE_BY_OTHERS == S_IXOTH,
              "base file permissions don't match unistd.h permissions");

enum RegularFileOrDeleteResult {
  kFailure = 0,      // Failed to delete whatever was at the path.
  kRegularFile = 1,  // Regular file existed and was unchanged.
  kEmpty = 2         // Anything that was at the path has been deleted.
};

// Checks if a regular file owned by |uid| and |gid| exists at |path|, otherwise
// deletes anything that might be at |path|. Returns a RegularFileOrDeleteResult
// enum indicating what is at |path| after the function finishes.
RegularFileOrDeleteResult RegularFileOrDelete(const base::FilePath& path,
                                              uid_t uid,
                                              gid_t gid) {
  // Check for symlinks by setting O_NOFOLLOW and checking for ELOOP. This lets
  // us use the safer fstat() instead of having to use lstat().
  base::ScopedFD scoped_fd(HANDLE_EINTR(openat(
      AT_FDCWD, path.value().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)));
  bool path_not_empty = (errno == ELOOP || scoped_fd != -1);

  // If there is a file/directory at |path|, see if it matches our criteria.
  if (scoped_fd != -1) {
    struct stat file_stat;
    if (fstat(scoped_fd.get(), &file_stat) != -1 &&
        S_ISREG(file_stat.st_mode) && file_stat.st_uid == uid &&
        file_stat.st_gid == gid) {
      return kRegularFile;
    }
  }

  // If we get here and anything was at |path|, try to delete it so we can put
  // our file there.
  if (path_not_empty) {
    if (!base::DeleteFile(path, true)) {
      PLOG(WARNING) << "Failed to delete entity at \"" << path.value() << '"';
      return kFailure;
    }
  }

  return kEmpty;
}

// Handles common touch functionality but also provides an optional |fd_out|
// so that any further modifications to the file (e.g. permissions) can safely
// use the fd rather than the path. |fd_out| will only be set if a new file
// is created, otherwise it will be unchanged.
// If |fd_out| is null, this function will close the file, otherwise it's
// expected that |fd_out| will close the file when it goes out of scope.
bool TouchFileInternal(const base::FilePath& path,
                       uid_t uid,
                       gid_t gid,
                       base::ScopedFD* fd_out) {
  RegularFileOrDeleteResult result = RegularFileOrDelete(path, uid, gid);
  switch (result) {
    case kFailure:
      return false;
    case kRegularFile:
      return true;
    case kEmpty:
      break;
  }

  // base::CreateDirectory() returns true if the directory already existed.
  if (!base::CreateDirectory(path.DirName())) {
    PLOG(WARNING) << "Failed to create directory for \"" << path.value() << '"';
    return false;
  }

  // Create the file as owner-only initially.
  base::ScopedFD scoped_fd(HANDLE_EINTR(openat(
      AT_FDCWD, path.value().c_str(),
      O_RDONLY | O_NOFOLLOW | O_CREAT | O_EXCL | O_CLOEXEC, kPermissions600)));
  if (scoped_fd == -1) {
    PLOG(WARNING) << "Failed to create file \"" << path.value() << '"';
    return false;
  }

  if (fd_out) {
    fd_out->swap(scoped_fd);
  }
  return true;
}

std::string GetRandomSuffix() {
  const int kBufferSize = 6;
  unsigned char buffer[kBufferSize];
  base::RandBytes(buffer, arraysize(buffer));
  std::string suffix;
  for (int i = 0; i < kBufferSize; ++i) {
    int random_value = buffer[i] % (2 * 26 + 10);
    if (random_value < 26) {
      suffix.push_back('a' + random_value);
    } else if (random_value < 2 * 26) {
      suffix.push_back('A' + random_value - 26);
    } else {
      suffix.push_back('0' + random_value - 2 * 26);
    }
  }
  return suffix;
}

}  // namespace

bool TouchFile(const base::FilePath& path,
               int new_file_permissions,
               uid_t uid,
               gid_t gid) {
  // Make sure |permissions| doesn't have any out-of-range bits.
  if (new_file_permissions & ~kPermissions777) {
    LOG(WARNING) << "Illegal permissions: " << new_file_permissions;
    return false;
  }

  base::ScopedFD scoped_fd;
  if (!TouchFileInternal(path, uid, gid, &scoped_fd)) {
    return false;
  }

  // scoped_fd is valid only if a new file was created.
  if (scoped_fd != -1 &&
      HANDLE_EINTR(fchmod(scoped_fd.get(), new_file_permissions)) == -1) {
    PLOG(WARNING) << "Failed to set permissions for \"" << path.value() << '"';
    base::DeleteFile(path, false);
    return false;
  }

  return true;
}

bool TouchFile(const base::FilePath& path) {
  // Use TouchFile() instead of TouchFileInternal() to explicitly set
  // permissions to 600 in case umask is set strangely.
  return TouchFile(path, kPermissions600, geteuid(), getegid());
}

bool WriteBlobToFile(const base::FilePath& path, const Blob& blob) {
  return WriteToFile(path, reinterpret_cast<const char*>(blob.data()),
                     blob.size());
}

bool WriteStringToFile(const base::FilePath& path, const std::string& data) {
  return WriteToFile(path, data.data(), data.size());
}

bool WriteToFile(const base::FilePath& path, const char* data, size_t size) {
  if (!base::DirectoryExists(path.DirName())) {
    if (!base::CreateDirectory(path.DirName())) {
      LOG(ERROR) << "Cannot create directory: " << path.DirName().value();
      return false;
    }
  }
  // base::WriteFile takes an int size.
  if (size > std::numeric_limits<int>::max()) {
    LOG(ERROR) << "Cannot write to " << path.value()
               << ". Data is too large: " << size << " bytes.";
    return false;
  }

  int data_written = base::WriteFile(path, data, size);
  return data_written == static_cast<int>(size);
}

bool SyncFileOrDirectory(const base::FilePath& path,
                         bool is_directory,
                         bool data_sync) {
  const base::TimeTicks start = base::TimeTicks::Now();
  data_sync = data_sync && !is_directory;

  int flags = (is_directory ? O_RDONLY | O_DIRECTORY : O_WRONLY);
  int fd = HANDLE_EINTR(open(path.value().c_str(), flags));
  if (fd < 0) {
    PLOG(WARNING) << "Could not open " << path.value() << " for syncing";
    return false;
  }
  // POSIX specifies EINTR as a possible return value of fsync() but not for
  // fdatasync().  To be on the safe side, it is handled in both cases.
  int result =
      (data_sync ? HANDLE_EINTR(fdatasync(fd)) : HANDLE_EINTR(fsync(fd)));
  if (result < 0) {
    PLOG(WARNING) << "Failed to sync " << path.value();
    close(fd);
    return false;
  }
  // close() may not be retried on error.
  result = IGNORE_EINTR(close(fd));
  if (result < 0) {
    PLOG(WARNING) << "Failed to close after sync " << path.value();
    return false;
  }

  const base::TimeDelta delta = base::TimeTicks::Now() - start;
  if (delta > kLongSync) {
    LOG(WARNING) << "Long " << (data_sync ? "fdatasync" : "fsync") << "() of "
                 << path.value() << ": " << delta.InSeconds() << " seconds";
  }

  return true;
}

bool WriteToFileAtomic(const base::FilePath& path,
                       const char* data,
                       size_t size,
                       mode_t mode) {
  if (!base::DirectoryExists(path.DirName())) {
    if (!base::CreateDirectory(path.DirName())) {
      LOG(ERROR) << "Cannot create directory: " << path.DirName().value();
      return false;
    }
  }
  std::string random_suffix = GetRandomSuffix();
  if (random_suffix.empty()) {
    PLOG(WARNING) << "Could not compute random suffix";
    return false;
  }
  std::string temp_name = path.AddExtension(random_suffix).value();
  int fd =
      HANDLE_EINTR(open(temp_name.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode));
  if (fd < 0) {
    PLOG(WARNING) << "Could not open " << temp_name << " for atomic write";
    unlink(temp_name.c_str());
    return false;
  }

  size_t position = 0;
  while (position < size) {
    ssize_t bytes_written =
        HANDLE_EINTR(write(fd, data + position, size - position));
    if (bytes_written < 0) {
      PLOG(WARNING) << "Could not write " << temp_name;
      close(fd);
      unlink(temp_name.c_str());
      return false;
    }
    position += bytes_written;
  }

  if (HANDLE_EINTR(fdatasync(fd)) < 0) {
    PLOG(WARNING) << "Could not fsync " << temp_name;
    close(fd);
    unlink(temp_name.c_str());
    return false;
  }
  if (close(fd) < 0) {
    PLOG(WARNING) << "Could not close " << temp_name;
    unlink(temp_name.c_str());
    return false;
  }

  if (rename(temp_name.c_str(), path.value().c_str()) < 0) {
    PLOG(WARNING) << "Could not close " << temp_name;
    unlink(temp_name.c_str());
    return false;
  }

  return true;
}

bool WriteBlobToFileAtomic(const base::FilePath& path,
                           const Blob& blob,
                           mode_t mode) {
  return WriteToFileAtomic(path, reinterpret_cast<const char*>(blob.data()),
                           blob.size(), mode);
}

}  // namespace brillo
