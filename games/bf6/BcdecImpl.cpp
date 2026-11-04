// The single translation unit that instantiates the vendored bcdec (MIT), shared with POE2/DR
// (third_party/bcdec/bcdec.h). Every other BF6 file includes bcdec.h for declarations only.
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
