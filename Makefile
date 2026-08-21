michacka: michacka.c
	gcc michacka.c -O2 -Wall -Wextra -o michacka -lm

test_michacka: michacka.c test_michacka.c
	gcc test_michacka.c -O2 -Wall -Wextra -o test_michacka -lm

test: michacka test_michacka
	./test_michacka

.PHONY: clean test
clean:
	rm -f michacka test_michacka
