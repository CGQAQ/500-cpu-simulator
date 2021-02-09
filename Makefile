CXX = clang++
CXXFLAGS = -o

ALL: sims assembler

sims: start.cpp
	$(CXX) $< $(CXXFLAGS) $@

assembler: assembler.cpp
	$(CXX) $< $(CXXFLAGS) $@


.PHONY: clean
clean:
	rm -f sims assembler
