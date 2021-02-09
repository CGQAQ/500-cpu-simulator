COMP 3370 Assignment 1 Sample programs
======================================

Sample programs and data
------------------------

This directory contains a variety of sample programs targeting the ISA described
in assignment 1 (files ending in `.asm`), with corresponding initial memory
states (files ending in `.dat`).

Sample simulator
----------------

A sample simulator (like what you're supposed to be making) has also been
supplied (`sim.out`). This program has been compiled on `rodents.cs.umanitoba.ca`
(the macOS lab on campus) and will *only* work on those machines (i.e., THIS
WILL NOT WORK ON YOUR COMPUTER).

To run the sample simulator, you need to pass it both the machine code and the
initial memory state:

    ./sim.out <code.o> <memory.dat>

To be clear: you must assemble the provided programs with the assembler *before*
trying to run them in the sample simulator (or your simulator).
