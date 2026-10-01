// Mirror of src/render_class.hpp (the canonical list; see it for the layer
// each class belongs to). scripts/check_constants.py fails the build if the
// names or numbers here differ from the C++ enum.
//
// The vertex attribute carries the class as a float. renderClass() rounds it
// back to an int, so every test below is an exact equality and the order the
// branches are written in no longer matters.

const int RC_WORLD = 0;
const int RC_WATER = 1;
const int RC_BULB = 2;
const int RC_MOON = 3;
const int RC_SKY = 4;
const int RC_DEBRIS = 5;
const int RC_MUZZLE = 6;
const int RC_INVENTORY_LATTICE = 7;
const int RC_WORLD_PICKUP = 8;

int renderClass(float attr) { return int(floor(attr + 0.5)); }
