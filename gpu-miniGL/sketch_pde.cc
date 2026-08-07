#include "psketch.hh"

// =============================================================================
//  sketch_pde.cc -- compile the selected Processing sketch
// =============================================================================
//
// The sketch is chosen by the DEMO make variable
// (`make DEMO=sketches/Basics/color/Radial_Gradient.pde`) and is plain C++,
// but Java's indifference to declaration order is not, so
// tools/pde_prototypes.py emits both halves: declarations first, then the
// sketch body with its class definitions hoisted above the functions and
// every .pde of the folder concatenated in dependency order.

#include "pde_prototypes.hh"
#include "pde_body.hh"

void sketch_setup()
{
	setup();
}

void sketch_draw()
{
	draw();
}
