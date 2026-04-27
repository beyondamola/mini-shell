shell: shell.c
	gcc -Wall -Wextra -g shell.c -o mini-shell

test: shell
	bash tests/test_mini_shell.sh

clean:
	rm -f mini-shell

