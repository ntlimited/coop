# Project: coop benchmarks

## Overview

This directory contains benchmarks for the project, including comparative benchmarks against the
'native'/kernel intermediated versions of the functionality we are handling in userspace.

## Coding Standards

### Style

Normal project style applies

### Best Practices
- Test functionality at various scales and accounting for synthetic behaviors - cache line
  alignment most commonly
- Benchmarks should be named in a manner that makes filtering for them easy in the compiled
  set across all files.
- Comparative benchmarks should be named so that they can easily be matched against eachother
