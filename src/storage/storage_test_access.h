#pragma once

#include "buffer_pool.h"

// Test-only access declarations, never included by core or public storage.h.
namespace tinydbms::storage::internal {
struct StorageTestAccess {
    static BufferPool* buffer_pool() noexcept;
    // FileManager remains available while Closing so tests can verify retained
    // file ownership after a failed close. BufferPool stays Open-only.
    static FileManager* file_manager() noexcept;
    // Only configurable while Closed. Defaults match production configuration.
    static bool configure(std::size_t capacity = 64,
        ReplacementPolicy policy = ReplacementPolicy::kFifo,
        BufferPool::WritePage write = {}, BufferPool::ReadPage read = {});
    static void fail_next_file_close(); // Error at the next PageFile close before stream cleanup.
};
}
