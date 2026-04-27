# Unix Shell (C)

A minimal Unix-like shell implemented in C to understand process creation, execution, and UNIX system primitives.

# Features
- Command parsing and argument tokenization
- Program creation using `fork()`
- Program execution via `execvp()`
- Built-in commands (`cd`)
- Quoting, escaping, and environment expansion
- Command chaining with `;`, `&&`, and `||`
- Piping with `|`
- Input and output redirection with `<`, `>`, and `>>`
- `$?` status expansion
- Exit status handling with `waitpid()`

# Examples
```bash
ls | grep shell
sort < input.txt > output.txt
echo "line" >> output.txt
```

## Build & Run
```bash
make
./mini-shell
```

## Tests
```bash
make test
```
