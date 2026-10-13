#pragma once

// Test-only partitions of the same native comparison and ownership checks.
enum class NativeMenuStateBucket {
    All, Small, Large, Stress,
    StressFilesNative, StressFilesRegistered, StressMixedNative, StressMixedRegistered
};

int runNativeMenuStateTests(NativeMenuStateBucket bucket = NativeMenuStateBucket::All);
