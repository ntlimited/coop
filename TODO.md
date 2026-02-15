# Probably important things
* Context allocation pooling; given that the size of a context stack can be set at runtime, we
  would likely need to enforce only (a) pooling allocations of that size and (b) having a config
  as part of the Cooperator constructor regarding how many to pool at max
* We also should benchmark the 'native' timeout support in I/O to see how that impacts performance
* We should benchmark the overhead of the scheduler in the basic "yield forever" case and measure
  the overhead of different numbers of contexts to just yield some number of times. This will
  evaluate the fixed ticker/io overhead as well as the out-of-thread submission queue overhead.

# Not so important but would be interesting
* Non-blocking DNS lookup
* OpenTelemetry support
