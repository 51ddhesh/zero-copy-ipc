// Copyright 2026 51ddhesh
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE or copy at
// http://www.boost.org/LICENSE_1_0.txt)

/*
    z_ipc::Histogram
    Fixed memory, O(1) record latency histogram with log linear bucketing, inspired by HdrHistogram
    Source: https://github.com/HdrHistogram/HdrHistogram
    Read more here: https://hdrhistogram.github.io/HdrHistogram/
*/

/*
    Design:
        The value range is divided into groups by powers of two.
        Within each group, there are `SubBucketCount` linear subdivisions.
        This gives relative precision proportional to magnitude - small values
        get finer granularity, large values get coarser buckets. 
        But the relative error is bounded.
*/

/*
    Memory:
        32 groups * 64 sub-buckets * 8 bytes = 16 KB
*/

/*
    Thread Safety:
        !! NOT thread safe 
*/

#pragma once


#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace z_ipc {



} // namespace z_ipc
