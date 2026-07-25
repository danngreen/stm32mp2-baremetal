# ipcc_hsem — passing work between the A35 cores and the M33

Three cores pass a token around a ring, forever:

```
A35 core 0  ->  A35 core 1  ->  M33  ->  A35 core 0
```

Each core stamps its name into a shared block of memory as the token comes past,
then hands it on. Core 0 prints a summary of each lap. 

This is `multicore_smp` (a second A35 core) and `copro_m33_embedded` (the M33
firmware embedded in the A35 image and started at runtime) combined, with the
two peripherals the STM32MP2 provides for inter-core work:

- **IPCC** (communication channel / doorbell) A set of mailbox channels between two processors.
  Raising a channel's flag interrupts the other side. Used here for both
  directions between the A35s and the M33.
- **HSEM** — (hardware semaphore) Any core can try to take one of the 16 hardware
  semaphores and exactly one wins. For this project, one semaphore guards the
  shared block, and a second guards the console UART, so three cores can print
  without their lines cutting into each other.

One asymmetry drives the design: IPCC and HSEM both treat the two A35 cores as a
*single* processor, so neither can address one A35 core specifically. Handing the
token from A35 core 0 to core 1 uses a software-generated interrupt (SGI)
instead — which is also why the ring is a ring, rather than each core talking to
every other.

The same asymmetry has a sharp edge on the locking side: because both A35 cores
are HSEM core ID 1, a semaphore taken by core ID alone does *not* exclude them
from each other. Each core therefore takes semaphores with its own PROCID (the
two-step lock), which is what the PROCID field is for.

## Checking that the lock really works

Alongside the ring, all three cores continuously take the semaphore just to
increment their own counter *and* a shared total. If the semaphore is doing its
job the total always equals the sum of the per-core counters, so each lap line
ends in `sum OK` — a broken lock would show up immediately as `SUM MISMATCH!`.
The `contended` figure counts how often a core found the semaphore already held,
which is what makes it a real test rather than an uncontended one.

Output looks like:

```
lap 7: A35_0 -> A35_1 -> M33  | locked bumps A35_0/A35_1/M33 = 4021/3887/152, total 8060 (sum OK), contended 921
```


## Building

The A35 build embeds the M33 binary, so `make` builds the M33 first and then the
A35 image that carries it.

