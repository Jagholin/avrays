#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN // Exclude rarely-used stuff from Windows headers
#include <windows.h>
#include <stdio.h>
#include <profileapi.h>
// Function to get monotonic clock time in seconds
double get_monotonic_time() {
    static LARGE_INTEGER frequency = { 0 };
    LARGE_INTEGER counter;

    // Initialize frequency only once
    if (frequency.QuadPart == 0) {
        if (!QueryPerformanceFrequency(&frequency)) {
            fprintf(stderr, "High-resolution performance counter not supported.\n");
            return -1.0;
        }
    }

    // Get current counter value
    if (!QueryPerformanceCounter(&counter)) {
        fprintf(stderr, "Failed to query performance counter.\n");
        return -1.0;
    }

    // Convert to seconds
    return (double)counter.QuadPart / (double)frequency.QuadPart;
}
#endif