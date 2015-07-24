// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chromeos/message_loops/base_message_loop.h>

#include <fcntl.h>
#include <unistd.h>

#include <base/bind.h>

#include <chromeos/location_logging.h>

using base::Closure;

namespace chromeos {

BaseMessageLoop::BaseMessageLoop(base::MessageLoopForIO* base_loop)
    : base_loop_(base_loop),
      weak_ptr_factory_(this) {}

BaseMessageLoop::~BaseMessageLoop() {
  for (auto& io_task : io_tasks_) {
    DVLOG_LOC(io_task.second.location(), 1)
        << "Removing file descriptor watcher task_id " << io_task.first
        << " leaked on BaseMessageLoop, scheduled from this location.";
    io_task.second.fd_watcher()->StopWatchingFileDescriptor();
  }

  // Note all pending canceled delayed tasks when destroying the message loop.
  size_t lazily_deleted_tasks = 0;
  for (const auto& delayed_task : delayed_tasks_) {
    if (delayed_task.second.closure.is_null()) {
      lazily_deleted_tasks++;
    } else {
      DVLOG_LOC(delayed_task.second.location, 1)
          << "Removing delayed task_id " << delayed_task.first
          << " leaked on BaseMessageLoop, scheduled from this location.";
    }
  }
  if (lazily_deleted_tasks) {
    LOG(INFO) << "Leaking " << lazily_deleted_tasks << " canceled tasks.";
  }
}

MessageLoop::TaskId BaseMessageLoop::PostDelayedTask(
    const tracked_objects::Location& from_here,
    const Closure &task,
    base::TimeDelta delay) {
  TaskId task_id =  NextTaskId();
  bool base_scheduled = base_loop_->task_runner()->PostDelayedTask(
      from_here,
      base::Bind(&BaseMessageLoop::OnRanPostedTask,
                 weak_ptr_factory_.GetWeakPtr(),
                 task_id),
      delay);
  DVLOG_LOC(from_here, 1) << "Scheduling delayed task_id " << task_id
                          << " to run in " << delay << ".";
  if (!base_scheduled)
    return MessageLoop::kTaskIdNull;

  delayed_tasks_.emplace(task_id,
                         DelayedTask{from_here, task_id, std::move(task)});
  return task_id;
}

MessageLoop::TaskId BaseMessageLoop::WatchFileDescriptor(
    const tracked_objects::Location& from_here,
    int fd,
    WatchMode mode,
    bool persistent,
    const Closure &task) {
  // base::MessageLoopForIO CHECKS that "fd >= 0", so we handle that case here.
  if (fd < 0)
    return MessageLoop::kTaskIdNull;

  base::MessageLoopForIO::Mode base_mode = base::MessageLoopForIO::WATCH_READ;
  switch (mode) {
    case MessageLoop::kWatchRead:
      base_mode = base::MessageLoopForIO::WATCH_READ;
      break;
    case MessageLoop::kWatchWrite:
      base_mode = base::MessageLoopForIO::WATCH_WRITE;
      break;
    default:
      return MessageLoop::kTaskIdNull;
  }

  TaskId task_id =  NextTaskId();
  auto it_bool = io_tasks_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(task_id),
      std::forward_as_tuple(from_here, this, task_id, persistent, task));
  // This should always insert a new element.
  DCHECK(it_bool.second);
  IOTask* new_io_task = &it_bool.first->second;

  bool scheduled = base_loop_->WatchFileDescriptor(
      fd, persistent, base_mode, new_io_task->fd_watcher(), new_io_task);

  DVLOG_LOC(from_here, 1)
      << "Watching fd " << fd << " for "
      << (mode == MessageLoop::kWatchRead ? "reading" : "writing")
      << (persistent ? " persistently" : " just once")
      << " as task_id " << task_id
      << (scheduled ? " successfully" : " failed.");

  if (!scheduled) {
    io_tasks_.erase(task_id);
    return MessageLoop::kTaskIdNull;
  }
  return task_id;
}

bool BaseMessageLoop::CancelTask(TaskId task_id) {
  if (task_id == kTaskIdNull)
    return false;
  auto delayed_task_it = delayed_tasks_.find(task_id);
  if (delayed_task_it == delayed_tasks_.end()) {
    // This might be an IOTask then.
    auto io_task_it = io_tasks_.find(task_id);
    if (io_task_it == io_tasks_.end())
      return false;

    DVLOG_LOC(io_task_it->second.location(), 1)
        << "Removing task_id " << task_id << " scheduled from this location.";
    // Destroying the FileDescriptorWatcher implicitly stops watching the file
    // descriptor.
    io_tasks_.erase(io_task_it);
    return true;
  }
  // A DelayedTask was found for this task_id at this point.

  // Check if the callback was already canceled but we have the entry in
  // delayed_tasks_ since it didn't fire yet in the message loop.
  if (delayed_task_it->second.closure.is_null())
    return false;

  DVLOG_LOC(delayed_task_it->second.location, 1)
      << "Removing task_id " << task_id << " scheduled from this location.";
  // We reset to closure to a null Closure to release all the resources
  // used by this closure at this point, but we don't remove the task_id from
  // delayed_tasks_ since we can't tell base::MessageLoopForIO to not run it.
  delayed_task_it->second.closure = Closure();

  return true;
}

bool BaseMessageLoop::RunOnce(bool may_block) {
  run_once_ = true;
  if (!may_block)
    base_loop_->RunUntilIdle();
  else
    base_loop_->Run();
  // If the flag was reset to false, it means a closure was run.
  if (!run_once_)
    return true;

  run_once_ = false;
  return false;
}

void BaseMessageLoop::Run() {
  base_loop_->Run();
}

void BaseMessageLoop::BreakLoop() {
  base_loop_->QuitNow();
}

MessageLoop::TaskId BaseMessageLoop::NextTaskId() {
  TaskId res;
  do {
    res = ++last_id_;
    // We would run out of memory before we run out of task ids.
  } while (!res ||
           delayed_tasks_.find(res) != delayed_tasks_.end() ||
           io_tasks_.find(res) != io_tasks_.end());
  return res;
}

void BaseMessageLoop::OnRanPostedTask(MessageLoop::TaskId task_id) {
  auto task_it = delayed_tasks_.find(task_id);
  DCHECK(task_it != delayed_tasks_.end());
  if (!task_it->second.closure.is_null()) {
    DVLOG_LOC(task_it->second.location, 1)
        << "Running delayed task_id " << task_id
        << " scheduled from this location.";
    // Mark the task as canceled while we are running it so CancelTask returns
    // false.
    Closure closure = std::move(task_it->second.closure);
    task_it->second.closure = Closure();
    closure.Run();

    // If the |run_once_| flag is set, it is because we are instructed to run
    // only once callback.
    if (run_once_) {
      run_once_ = false;
      base_loop_->QuitNow();
    }
  }
  delayed_tasks_.erase(task_it);
}

BaseMessageLoop::IOTask::IOTask(const tracked_objects::Location& location,
                                BaseMessageLoop* loop,
                                MessageLoop::TaskId task_id,
                                bool persistent,
                                const Closure& task)
    : location_(location), loop_(loop), task_id_(task_id),
      persistent_(persistent), closure_(task) {}

void BaseMessageLoop::IOTask::OnFileCanReadWithoutBlocking(int fd) {
  OnFileReady(fd);
}

void BaseMessageLoop::IOTask::OnFileCanWriteWithoutBlocking(int fd) {
  OnFileReady(fd);
}

void BaseMessageLoop::IOTask::OnFileReady(int fd) {
  // We can't access |this| after running the |closure_| since it could call
  // CancelTask on its own task_id, so we copy the members we need now.
  BaseMessageLoop* loop_ptr = loop_;

  DVLOG_LOC(location_, 1)
      << "Running task_id " << task_id_
      << " for watching file descriptor " << fd
      << ", scheduled from this location.";

  if (persistent_) {
    // In the persistent case we just run the callback. If this callback cancels
    // the task id, we can't access |this| anymore.
    closure_.Run();
  } else {
    // This will destroy |this|, the fd_watcher and therefore stop watching this
    // file descriptor.
    Closure closure_copy = std::move(closure_);
    loop_->io_tasks_.erase(task_id_);
    // Run the closure from the local copy we just made.
    closure_copy.Run();
  }

  if (loop_ptr->run_once_) {
    loop_ptr->run_once_ = false;
    loop_ptr->base_loop_->QuitNow();
  }
}

}  // namespace chromeos