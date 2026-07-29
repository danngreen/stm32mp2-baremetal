// Cache-sanitizer stub for loading images via OpenOCD/ST-LINK.
//
// After OpenOCD loads a new image into DDR, the A35 can intermittently fetch
// stale instructions from the previously-running image at those addresses,
// even with SCTLR.{M,C,I}=0 at the parked state. A ranged dc-civac plus
// ic-iallu proved insufficient, so this does the full cold-boot-style
// sanitize: set/way clean+invalidate of every data/unified cache level,
// I-cache + branch predictor invalidate, TLB invalidates, and cold-boot-like
// EL1 sysregs. Runs from an untouched address (default 0x9FF00000) so its own
// fetches are never stale.
//
// The debugger sets:
//   x20 = entry point of the loaded image
// and resumes execution at the stub base. (x0/x1 are clobbered; kept as
// as inputs for compatibility: they are ignored.)
//
// Must run at EL3 (the parked "Ready" state).

    .section .text, "ax"
    .global _stub_start
_stub_start:
    dsb     sy

    // Clean+invalidate all data/unified cache levels by set/way (to PoC).
    // Same algorithm as debug_load.
    mrs     x0, clidr_el1
    and     w3, w0, #0x07000000     // w3 = Level of Coherence << 1
    lsr     w3, w3, #23
    cbz     w3, 5f
    mov     w10, #0                 // w10 = 2 * cache level
    mov     w8, #1
1:  add     w2, w10, w10, lsr #1    // w2 = 3 * cache level
    lsr     w1, w0, w2              // w1 = cache type for this level
    and     w1, w1, #0x7
    cmp     w1, #2
    b.lt    4f                      // no data/unified cache at this level
    msr     csselr_el1, x10
    isb
    mrs     x1, ccsidr_el1
    and     w2, w1, #7
    add     w2, w2, #4              // w2 = log2(line bytes)
    ubfx    w4, w1, #3, #10         // w4 = ways - 1
    clz     w5, w4                  // w5 = way field shift
    lsl     w9, w4, w5              // w9 = max way, in position
    lsl     w16, w8, w5             // w16 = way decrement
2:  ubfx    w7, w1, #13, #15        // w7 = sets - 1
    lsl     w7, w7, w2              // in position
    lsl     w17, w8, w2             // w17 = set decrement
3:  orr     w11, w10, w9
    orr     w11, w11, w7
    dc      cisw, x11               // clean+invalidate line
    subs    w7, w7, w17
    b.ge    3b
    subs    w9, w9, w16
    b.ge    2b
4:  add     w10, w10, #2
    cmp     w3, w10
    b.gt    1b
5:  mov     x10, #0
    msr     csselr_el1, x10
    dsb     sy
    isb

    ic      iallu                   // invalidate I-cache + branch predictor
    tlbi    alle3
    tlbi    alle1
    msr     sctlr_el1, xzr          // cold-boot-like EL1: MMU/caches off
    msr     vbar_el1, xzr
    dsb     sy
    isb
    br      x20

    // Alternate entry target for x2: park here (e.g. to sanitize before loading)
    .global _stub_park
_stub_park:
    b       _stub_park
