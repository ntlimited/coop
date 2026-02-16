# Probably important things
* Context allocation pooling; given that the size of a context stack can be set at runtime, we
  would likely need to enforce only (a) pooling allocations of that size and (b) having a config.
  Almost certainly guard pages should also be part of this; we can imagine applications with big,
  hairy tasks that need extra protection but also massive numbers of smaller tasks with well
  bounded behaviors that do not.

# Not so important but would be interesting
* OpenTelemetry support
