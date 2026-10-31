# Mini Unix Shell in C

## Build & run
    make
    ./myshell

## Features
- Run programs (`fork` + `execvp`)
- Pipes: `ls | grep c | wc -l`
- Redirection: `<`, `>`, `>>`
- Background jobs: `sleep 10 &` (zombies reaped before each prompt)
- Built-ins: `cd`, `exit`, `help`
- Ctrl+C stops the foreground job, not the shell; Ctrl+D exits

## How it works
- `parse_line` tokenizes and builds a `struct command` per pipeline stage
- `run_pipeline` creates N-1 pipes, forks N children, wires stdin/stdout with `dup2`
- Parent closes every pipe end (otherwise readers never get EOF), then `waitpid`s
- Children reset SIGINT to default before `exec`

## Test with Valgrind
    valgrind --leak-check=full ./myshell

## Ideas to extend
- Quotes and environment variables (`$HOME`)
- Job control (`jobs`, `fg`, `bg`, Ctrl+Z)
- Command history, tab completion
