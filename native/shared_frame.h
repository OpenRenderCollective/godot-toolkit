#pragma once
#include <cstdint>

// Private, versioned editor ABI. Separate from the upstream Remix API table.
// Descriptors returned here belong to the caller: file descriptors sent with
// SCM_RIGHTS on Linux, NT handles duplicated into the editor on Windows.
struct EditorSharedFrame {
  uint32_t version = 1;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t memory_type = 0;
  uint64_t allocation_size = 0;
  uint8_t device_uuid[16]{};
#ifdef _WIN32
  void* memory_handle = nullptr;
  void* ready_handle = nullptr;
  void* released_handle = nullptr;
#else
  int memory_fd = -1;
  int ready_fd = -1;
  int released_fd = -1;
#endif
};
using ConfigureSharedFrame = int (*)(EditorSharedFrame*);
