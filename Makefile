michacka: michacka.c
	gcc michacka.c -O2 -Wall -Wextra -o michacka -lm

test_michacka: michacka.c test_michacka.c
	gcc test_michacka.c -O2 -Wall -Wextra -o test_michacka -lm

test: michacka test_michacka
	./test_michacka

smoke: michacka
	@mkdir -p test_env/.config test_env/mus test_env/fld
	@touch test_env/mus/track1.wav test_env/fld/env1.wav
	@printf 'mus=%s/test_env/mus\nfld=%s/test_env/fld\n' "$(CURDIR)" "$(CURDIR)" > test_env/.config/michacka.conf
	@HOME=$(CURDIR)/test_env ./michacka day 42 --parts 1 --dry-run --out test_dryrun >/dev/null
	@test -f test_dryrun_part01_music.edl -a -f test_dryrun_part01_field.edl
	@echo "smoke ok: EDLs generated"
	@rm -rf test_env test_dryrun_*

.PHONY: clean test smoke
clean:
	rm -f michacka test_michacka
	rm -rf test_env test_dryrun_*
