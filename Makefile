michacka: michacka.c
	gcc michacka.c -O2 -Wall -Wextra -o michacka -lm

.PHONY: clean
clean:
	rm -f michacka
