#include <cstdio>
#include <vector>
#include "histogram.h"

static const char *MNAME[] = { "NAIVE", "FORWARD", "BANKED", "UNSAFE-DEPENDENCE" };

static int run_case(const std::vector<unsigned char> &data, const char *name)
{
    byte_stream_t src("src");
    for (size_t i = 0; i < data.size(); ++i) {
        byte_axis_t w;
        w.data = data[i];
        AXIS_SET_KEEP(w);
        w.user = (i == 0);
        w.last = (i == data.size() - 1);
        w.id = 0; w.dest = 0;
        src.write(w);
    }

    std::vector<count_t> got(NBINS, 0);
    histogram(src, got.data(), (ap_uint<32>)data.size());

    std::vector<unsigned> exp(NBINS, 0);
    for (size_t i = 0; i < data.size(); ++i) exp[data[i]]++;

    int errs = 0;
    unsigned long total = 0;
    for (int b = 0; b < NBINS; ++b) {
        total += (unsigned long)got[b];
        if ((unsigned)got[b] != exp[b]) {
            if (errs < 5)
                printf("  [%s] bin %d: got %u expected %u\n",
                       name, b, (unsigned)got[b], exp[b]);
            ++errs;
        }
    }
    if (total != data.size())
        printf("  [%s] total count %lu != %zu samples  <-- counts were LOST\n",
               name, total, data.size());
    printf("  [%-14s] %zu samples, %d wrong bins  %s\n",
           name, data.size(), errs, errs ? "FAIL" : "ok");
    return errs;
}

int main()
{
    printf("=== METHOD=%d (%s) NBINS=%d ===\n", METHOD, MNAME[METHOD], NBINS);
    int errs = 0;

    /* 1. Uniform-ish random data. Collisions between ADJACENT samples are
     *    rare, so this case passes even with a broken dependence handling.
     *    That is precisely why it is not sufficient. */
    {
        std::vector<unsigned char> d(4096);
        unsigned s = 7u;
        for (size_t i = 0; i < d.size(); ++i) { s = s*1103515245u+12345u; d[i] = (s >> 16) & 0xFF; }
        errs += run_case(d, "random");
    }

    /* 2. A RUN of identical samples. Every iteration collides with the
     *    previous one, so this is the case the dependency exists for.
     *    METHOD=3 loses counts here and nowhere else. */
    {
        std::vector<unsigned char> d(1000, 42);
        errs += run_case(d, "run-of-same");
    }

    /* 3. Alternating pair -- collides at distance 2 rather than 1, which
     *    catches a forwarding implementation that only remembers one step
     *    when the memory latency is longer than one cycle. */
    {
        std::vector<unsigned char> d(1000);
        for (size_t i = 0; i < d.size(); ++i) d[i] = (i & 1) ? 7 : 200;
        errs += run_case(d, "alternating");
    }

    /* 4. Short runs of each value -- the realistic middle ground. */
    {
        std::vector<unsigned char> d;
        for (int v = 0; v < 64; ++v)
            for (int r = 0; r < 9; ++r) d.push_back((unsigned char)v);
        errs += run_case(d, "short-runs");
    }

    /* 5. Edge bins. */
    {
        std::vector<unsigned char> d;
        for (int r = 0; r < 50; ++r) { d.push_back(0); d.push_back(255); }
        errs += run_case(d, "edge-bins");
    }

#if METHOD == 3
    printf("\nNOTE: METHOD=3 asserts a dependency that demonstrably exists.\n");
    printf("      It reaches II=1 and LOSES COUNTS on repeated values.\n");
    printf("      The failures above are the lesson, not a bug in the test.\n");
    if (errs) { printf("\n*** FAIL (expected for METHOD=3): %d errors\n", errs); return 1; }
    printf("\n*** UNEXPECTED PASS -- csim does not model the RTL hazard;\n");
    printf("    run cosim to see METHOD=3 fail for real.\n");
    return 0;
#else
    if (errs) { printf("\n*** FAIL: %d errors\n", errs); return 1; }
    printf("\n*** PASS\n");
    return 0;
#endif
}
