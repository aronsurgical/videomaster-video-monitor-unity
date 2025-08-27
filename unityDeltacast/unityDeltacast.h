#pragma once

#ifdef _WIN32
#  define UNITYDLL_EXPORT __declspec(dllexport)
#else
#  define UNITYDLL_EXPORT
#endif

// C API: functions must be extern "C" to avoid C++ name mangling
extern "C" {

// Example: initialize something
UNITYDLL_EXPORT void InitLibrary();

// Example: add two numbers
UNITYDLL_EXPORT int AddInts(int a, int b);

// Example: pass a string back to Unity (caller copies it!)
UNITYDLL_EXPORT const char* GetMessage();

}