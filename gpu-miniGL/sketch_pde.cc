#include "psketch.hh"

// =============================================================================
//  sketch_pde.cc -- compile the selected Processing sketch
// =============================================================================
//
// PSKETCH_DEMO is the .pde path from the DEMO make variable
// (`make DEMO=sketches/Basics/color/Radial_Gradient.pde`). The sketch is
// plain C++ except that Java ignores definition order, so generated forward
// declarations come first -- exactly what Processing's own preprocessor does.

#include "pde_prototypes.hh"

#include PSKETCH_DEMO

void sketch_setup()
{
	setup();
}

void sketch_draw()
{
	draw();
}
