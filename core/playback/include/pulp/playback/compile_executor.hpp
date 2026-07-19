#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

namespace pulp::playback {

struct CompileSliceBudget {
    std::chrono::steady_clock::time_point deadline;
    std::size_t max_work_units = 256;
};

enum class CompileTaskStatus { Pending, Complete };

class CompileTask {
  public:
    virtual ~CompileTask() = default;
    virtual CompileTaskStatus run_slice(const CompileSliceBudget& budget) noexcept = 0;
};

class CompileExecutor {
  public:
    virtual ~CompileExecutor() = default;
    virtual bool submit(std::unique_ptr<CompileTask> task,
                        std::chrono::steady_clock::time_point not_before) = 0;
};

class DeferredCompileExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task,
                std::chrono::steady_clock::time_point not_before) override;
    std::size_t run_for(std::chrono::microseconds budget, std::size_t max_work_units = 256);
    std::size_t pending_count() const noexcept { return tasks_.size(); }

  private:
    struct Entry {
        std::chrono::steady_clock::time_point not_before;
        std::unique_ptr<CompileTask> task;
    };
    std::vector<Entry> tasks_;
};

class WorkerCompileExecutor final : public CompileExecutor {
  public:
    WorkerCompileExecutor();
    ~WorkerCompileExecutor() override;
    WorkerCompileExecutor(const WorkerCompileExecutor&) = delete;
    WorkerCompileExecutor& operator=(const WorkerCompileExecutor&) = delete;
    bool submit(std::unique_ptr<CompileTask> task,
                std::chrono::steady_clock::time_point not_before) override;
    bool supported() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::playback
