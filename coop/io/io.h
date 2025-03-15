#pragma once

#include "accept.h"
#include "recv.h"
#include "send.h"

// coop::io offers a very direct abstraction for blocking i/o and its user facing unit is the
// Descriptor, which wraps a file descriptor. Methods are made available as e.g. coop::io::Recv
// with various parameter overloads, starting with a near carbon copy of the 'real' version
// replacing the fd with a Descriptor.
//
// A lower-level API is available through Handle, which allows doing things like submitting
// multiple operations from a single context and compositional mechanics using coordinators.
//
