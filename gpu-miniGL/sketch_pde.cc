#include "psketch.hh"

// =============================================================================
//  sketch_pde.cc -- compile the selected Processing sketch
// =============================================================================
//
// The sketch is chosen by the DEMO make variable
// (`make DEMO=sketches/Basics/color/Radial_Gradient.pde`) and is plain C++,
// but two Processing-isms need handling first, both by
// tools/pde_prototypes.py: Java ignores declaration order (so forward
// declarations come first), and a sketch folder's several .pde "tabs" are one
// translation unit, not separate ones (so every .pde in the folder is
// included here, ordered so a class is defined before it is used).

#include "pde_prototypes.hh"
#include "pde_includes.hh"

void sketch_setup()
{
	setup();
}

void sketch_draw()
{
	draw();
}
