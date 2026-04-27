#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <fcntl.h>
#include <ctype.h>

#define MAX_ARGS 64
#define MAX_INPUT 256
#define MAX_TOKENS 128
#define MAX_CMDS 16

#define RESET "\x1b[0m"
#define MAGENTA "\x1b[35m"

typedef struct {
	char *argv[MAX_ARGS];
	int argc;
	char *input_file;
	char *output_file;
	bool append_output;
} Command;

static void init_command(Command *cmd){
	cmd->argc = 0;
	cmd->input_file = NULL;
	cmd->output_file = NULL;
	cmd->append_output = false;
	for (int i = 0; i < MAX_ARGS; ++i){
		cmd->argv[i] = NULL;
	}
}

static int tokenize(char *line, char *scratch, size_t scratch_size, char *tokens[], int max_tokens){
	size_t out = 0;
	bool last_was_space = true;

	for (size_t i = 0; line[i] != '\0' && line[i] != '\n'; ++i){
		unsigned char c = (unsigned char)line[i];
		if (isspace(c)){
			if (!last_was_space){
				if (out + 1 >= scratch_size){
					return -1;
				}
				scratch[out++] = ' ';
				last_was_space = true;
			}
			continue;
		}

		if (c == '>' && line[i + 1] == '>'){
			if (out + 4 >= scratch_size){
				return -1;
			}
			if (!last_was_space){
				scratch[out++] = ' ';
			}
			scratch[out++] = '>';
			scratch[out++] = '>';
			scratch[out++] = ' ';
			last_was_space = true;
			++i;
			continue;
		}

		if (c == '|' || c == '<' || c == '>'){
			if (out + 3 >= scratch_size){
				return -1;
			}
			if (!last_was_space){
				scratch[out++] = ' ';
			}
			scratch[out++] = (char)c;
			scratch[out++] = ' ';
			last_was_space = true;
			continue;
		}

		if (out + 1 >= scratch_size){
			return -1;
		}
		scratch[out++] = (char)c;
		last_was_space = false;
	}

	if (out > 0 && scratch[out - 1] == ' '){
		--out;
	}
	scratch[out] = '\0';

	int count = 0;
	char *saveptr = NULL;
	char *token = strtok_r(scratch, " \t\r\n", &saveptr);
	while (token != NULL){
		if (count >= max_tokens - 1){
			return -1;
		}
		tokens[count++] = token;
		token = strtok_r(NULL, " \t\r\n", &saveptr);
	}
	tokens[count] = NULL;
	return count;
}

static int parse_commands(char *tokens[], int token_count, Command commands[], int max_commands){
	int command_count = 0;
	init_command(&commands[0]);

	for (int i = 0; i < token_count; ++i){
		char *token = tokens[i];

		if (strcmp(token, "|") == 0){
			if (commands[command_count].argc == 0){
				fprintf(stderr, "mini-shell: invalid null command\n");
				return -1;
			}
			commands[command_count].argv[commands[command_count].argc] = NULL;
			++command_count;
			if (command_count >= max_commands){
				fprintf(stderr, "mini-shell: too many pipeline stages\n");
				return -1;
			}
			init_command(&commands[command_count]);
			continue;
		}

		if (strcmp(token, "<") == 0 || strcmp(token, ">") == 0 || strcmp(token, ">>") == 0){
			if (i + 1 >= token_count){
				fprintf(stderr, "mini-shell: redirection missing target file\n");
				return -1;
			}
			char *filename = tokens[++i];
			if (strcmp(token, "<") == 0){
				commands[command_count].input_file = filename;
			} else {
				commands[command_count].output_file = filename;
				commands[command_count].append_output = (strcmp(token, ">>") == 0);
			}
			continue;
		}

		if (commands[command_count].argc >= MAX_ARGS - 1){
			fprintf(stderr, "mini-shell: too many arguments\n");
			return -1;
		}
		commands[command_count].argv[commands[command_count].argc++] = token;
	}

	if (commands[command_count].argc == 0){
		fprintf(stderr, "mini-shell: invalid null command\n");
		return -1;
	}
	commands[command_count].argv[commands[command_count].argc] = NULL;
	return command_count + 1;
}

static bool apply_input_redirection(const char *path){
	int fd = open(path, O_RDONLY);
	if (fd < 0){
		perror(path);
		return false;
	}
	if (dup2(fd, STDIN_FILENO) < 0){
		perror("dup2");
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

static bool apply_output_redirection(const char *path, bool append){
	int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
	int fd = open(path, flags, 0644);
	if (fd < 0){
		perror(path);
		return false;
	}
	if (dup2(fd, STDOUT_FILENO) < 0){
		perror("dup2");
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

static bool run_single_command(Command *cmd){
	pid_t pid = fork();
	if (pid == -1) {perror("Fork failed - Reprompt\n"); return false;}

	if (pid == 0) {
		if (cmd->input_file != NULL && !apply_input_redirection(cmd->input_file)){
			_exit(1);
		}
		if (cmd->output_file != NULL && !apply_output_redirection(cmd->output_file, cmd->append_output)){
			_exit(1);
		}
		execvp(cmd->argv[0], cmd->argv);
		perror(cmd->argv[0]);
		_exit(127);
	}
	else {
		int status;
		waitpid(pid, &status, 0);
		if (WIFSIGNALED(status)){perror("Child Process Error - Signal"); return false;}
	}
	return true;
}

static bool run_pipeline(Command commands[], int command_count){
	pid_t pids[MAX_CMDS];
	int prev_read = -1;

	for (int i = 0; i < command_count; ++i){
		int pipefd[2] = {-1, -1};
		if (i < command_count - 1 && pipe(pipefd) < 0){
			perror("pipe");
			if (prev_read != -1){
				close(prev_read);
			}
			return false;
		}

		pid_t pid = fork();
		if (pid < 0){
			perror("fork");
			if (prev_read != -1){
				close(prev_read);
			}
			if (pipefd[0] != -1){
				close(pipefd[0]);
			}
			if (pipefd[1] != -1){
				close(pipefd[1]);
			}
			return false;
		}

		if (pid == 0){
			if (prev_read != -1 && commands[i].input_file == NULL){
				if (dup2(prev_read, STDIN_FILENO) < 0){
					perror("dup2");
					_exit(1);
				}
			}
			if (commands[i].input_file != NULL && !apply_input_redirection(commands[i].input_file)){
				_exit(1);
			}

			if (commands[i].output_file != NULL){
				if (!apply_output_redirection(commands[i].output_file, commands[i].append_output)){
					_exit(1);
				}
			} else if (i < command_count - 1){
				if (dup2(pipefd[1], STDOUT_FILENO) < 0){
					perror("dup2");
					_exit(1);
				}
			}

			if (prev_read != -1){
				close(prev_read);
			}
			if (pipefd[0] != -1){
				close(pipefd[0]);
			}
			if (pipefd[1] != -1){
				close(pipefd[1]);
			}

			execvp(commands[i].argv[0], commands[i].argv);
			perror(commands[i].argv[0]);
			_exit(127);
		}

		pids[i] = pid;
		if (prev_read != -1){
			close(prev_read);
		}
		if (pipefd[1] != -1){
			close(pipefd[1]);
		}
		prev_read = pipefd[0];
	}

	if (prev_read != -1){
		close(prev_read);
	}

	bool ok = true;
	for (int i = 0; i < command_count; ++i){
		int status;
		if (waitpid(pids[i], &status, 0) < 0){
			perror("waitpid");
			ok = false;
			continue;
		}
		if (WIFSIGNALED(status)){
			perror("Child Process Error - Signal");
			ok = false;
		}
	}
	return ok;
}

int main (){
	char line[MAX_INPUT];
	char scratch[MAX_INPUT * 2];
	char *tokens[MAX_TOKENS];
	Command commands[MAX_CMDS];

	while(1){
		printf(MAGENTA "mini-shell> " RESET);

		// Ctl + D signal
		if(fgets(line, sizeof(line), stdin) == NULL){break;}

		int token_count = tokenize(line, scratch, sizeof(scratch), tokens, MAX_TOKENS);
		if (token_count < 0){
			fprintf(stderr, "mini-shell: input too long\n");
			continue;
		}
		if (token_count == 0){
			continue;
		}

		int command_count = parse_commands(tokens, token_count, commands, MAX_CMDS);
		if (command_count < 0){
			continue;
		}

		if (command_count == 1){
			char **args = commands[0].argv;
			if (args[0] == NULL){
				continue;
			}

			// checks for exit signal
			if (strcmp(args[0], "exit") == 0){break;}

			// change directory builtin
			if (strcmp(args[0], "cd") == 0){
				const char *target = args[1] != NULL ? args[1] : getenv("HOME");
				if (target == NULL || chdir(target) != 0){
					perror("cd");
				}
				continue;
			}

			run_single_command(&commands[0]);
			continue;
		}

		run_pipeline(commands, command_count);
	}
	return 0;
}
