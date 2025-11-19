# dyninst-tests
Unit tests for Dyninst binary analysis.

1. cfg-parse -- dumps the raw data that dyninst presents to hpcstruct,
including functions, basic blocks, statements, line map and inline
sequences.  Tests for non-det (and thus incorrect) output from many
threads.

2. unknown-x86 -- tests for x86_64 instructions that are either
unknown by dyninst or for which dyninst gets the wrong length.
