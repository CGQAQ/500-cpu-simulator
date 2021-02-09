CXX = clang++

ALL:
	sims

sims: start.cpp
	$(CXX) $< -o $@

assembler: assembler.cpp
	$(CXX) $< -o $@


.PHONY: clean
clean:
	rm -f sims assembler
