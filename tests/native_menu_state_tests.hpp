#pragma once

// Test-only partitions of the same native comparison and ownership checks.
enum class NativeMenuStateBucket { All, Small, Large, Stress };

int runNativeMenuStateTests(NativeMenuStateBucket bucket = NativeMenuStateBucket::All);
