# IPCC — Inter-Processor Communications Channel

A35 core 0 asks the M33 to do some "work" (look up a string by index) via
a mailbox in shared memory, using the IPCC peripheral for communication.

```
A35_0: writes index into the mailbox
A35_0 -> M33    IPCC1 channel 1  "there is work for you"
  M33: writes the matching string into the mailbox
  M33 -> A35_0  IPCC1 channel 2  "your result is ready"
A35_0: prints the result
```

```
A35_0: asked for 1, M33 answered "Orange" (1 served)
A35_0: asked for 2, M33 answered "Blue" (2 served)
...
A35_0: asked for 9, M33 answered "no such color" (9 served)
```

Startup of the M33 core is the same as how it's done in the
`copro_m33_embedded` project (the M33 firmware is embedded in the A35 image and
started at runtime) 

In the real world we'd probably use an HSEM around access to shared memory,
but this example demonstrates the IPCC peripheral as the only method of
synchronization. Ownership simply follows the notifications: the mailbox
  belongs to A35_0 until it raises channel 1, then to the M33 until it raises
  channel 2. Each side only touches the mailbox while it holds ownership.

IPCC works like this:
- A core sets its own channel flag to notify the other side.
- The receiver clears it in the ISR to acknowledge

When run, you should see:

```
IPCC request/response demo (A35 <-> M33)
===============================================

A35_0: starting M33 (7868 bytes in SRAM2)
M33: online, serving 8 colors
A35_0: M33 is online

A35_0: asked for 1, M33 answered "Orange" (1 served)
A35_0: asked for 2, M33 answered "Blue" (2 served)
A35_0: asked for 3, M33 answered "Green" (3 served)
A35_0: asked for 4, M33 answered "Crimson" (4 served)
A35_0: asked for 5, M33 answered "Violet" (5 served)
A35_0: asked for 6, M33 answered "Silver" (6 served)
A35_0: asked for 7, M33 answered "Teal" (7 served)
A35_0: asked for 8, M33 answered "Chartreuse" (8 served)
A35_0: asked for 9, M33 answered "no such color" (9 served)
A35_0: asked for 10, M33 answered "no such color" (10 served)
A35_0: asked for 11, M33 answered "no such color" (11 served)
Done.
```
