#pragma once

namespace coop
{

struct Coordinator;
struct Context;

namespace network
{

struct Router;

// Dial is just a dumb wrapper for internal purposes right now; realistically, I don't think there
// is any real reason to make the "wait for the connection" part natively async, when using the event
// router will allow for the | OUT event anyway.
//
int Dial(
    const char* hostname,
    int port);

} // end namespace coop::network
} // end namespace coop
