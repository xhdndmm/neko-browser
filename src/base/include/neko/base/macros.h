#pragma once

// Compile-time macros shared across the project.

// Deletes the copy constructor and copy assignment operator.
#define NEKO_DISALLOW_COPY(ClassName)        \
  ClassName(const ClassName&) = delete;      \
  ClassName& operator=(const ClassName&) = delete;

// Deletes the move constructor and move assignment operator.
#define NEKO_DISALLOW_MOVE(ClassName) \
  ClassName(ClassName&&) = delete;    \
  ClassName& operator=(ClassName&&) = delete;

// Deletes all four special member functions.
#define NEKO_DISALLOW_COPY_AND_MOVE(ClassName) \
  NEKO_DISALLOW_COPY(ClassName)                \
  NEKO_DISALLOW_MOVE(ClassName)
