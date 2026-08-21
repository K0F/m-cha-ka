michacka: michacka.c
	gcc michacka.c -O2 -Wall -Wextra -o michacka -lm

.PHONY: clean test
clean:
	rm -f michacka test_dryrun_*
	rm -rf test_env

test: michacka
	@echo "Running basic tests..."
	@mkdir -p test_env/mus test_env/fld
	@touch test_env/mus/track1.wav test_env/fld/env1.wav
	@./michacka --dry-run --parts 1 --part-len 20 --mus test_env/mus --fld test_env/fld --out test_dryrun > /dev/null 2>&1
	@if [ -f test_dryrun_part01_music.edl ] && [ -f test_dryrun_part01_field.edl ]; then \
		echo "Test passed: EDL files generated successfully."; \
		rm -rf test_env test_dryrun_*; \
	else \
		echo "Test failed: EDL files not found."; \
		rm -rf test_env test_dryrun_*; \
		exit 1; \
	fi
