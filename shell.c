#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ARGS 64
#define MAX_INPUT 1024
#define MAX_TOKENS 256
#define MAX_CMDS 16
#define MAX_JOBS 16

#define RESET "\x1b[0m"
#define MAGENTA "\x1b[35m"

typedef struct {
	char *argv[MAX_ARGS];
	int argc;
	char *input_file;
	char *output_file;
	bool append_output;
} Command;

typedef enum {
	CONNECTOR_SEQUENCE,
	CONNECTOR_AND,
	CONNECTOR_OR,
	CONNECTOR_END
} Connector;

typedef struct {
	Command commands[MAX_CMDS];
	int command_count;
	Connector connector_after;
} Job;

static int g_last_status = 0;

static void init_command(Command *cmd){
	cmd->argc = 0;
	cmd->input_file = NULL;
	cmd->output_file = NULL;
	cmd->append_output = false;
	for (int i = 0; i < MAX_ARGS; ++i){
		cmd->argv[i] = NULL;
	}
}

static void init_job(Job *job){
	job->command_count = 0;
	job->connector_after = CONNECTOR_END;
	for (int i = 0; i < MAX_CMDS; ++i){
		init_command(&job->commands[i]);
	}
}

static bool is_name_start(int c){
	return isalpha(c) || c == '_';
}

static bool is_name_char(int c){
	return isalnum(c) || c == '_';
}

static bool is_operator_token(const char *token){
	return strcmp(token, "|") == 0 || strcmp(token, "<") == 0 || strcmp(token, ">") == 0 ||
		strcmp(token, ">>") == 0 || strcmp(token, ";") == 0 || strcmp(token, "&&") == 0 ||
		strcmp(token, "||") == 0;
}

static bool is_connector_token(const char *token){
	return strcmp(token, ";") == 0 || strcmp(token, "&&") == 0 || strcmp(token, "||") == 0;
}

static bool append_char(char *scratch, size_t scratch_size, size_t *out, char c){
	if (*out + 1 >= scratch_size){
		return false;
	}
	scratch[(*out)++] = c;
	return true;
}

static bool append_string(char *scratch, size_t scratch_size, size_t *out, const char *text){
	if (text == NULL){
		return true;
	}
	for (size_t i = 0; text[i] != '\0'; ++i){
		if (!append_char(scratch, scratch_size, out, text[i])){
			return false;
		}
	}
	return true;
}

static bool expand_variable(const char *line, size_t *index, char *scratch, size_t scratch_size, size_t *out){
	size_t i = *index;

	if (line[i + 1] == '?'){
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%d", g_last_status);
		if (!append_string(scratch, scratch_size, out, buffer)){
			return false;
		}
		*index = i + 2;
		return true;
	}

	if (line[i + 1] == '{'){
		size_t j = i + 2;
		size_t start = j;
		while (line[j] != '\0' && line[j] != '}'){
			if (!is_name_char((unsigned char)line[j])){
				return false;
			}
			++j;
		}
		if (line[j] != '}'){
			return false;
		}

		char name[MAX_ARGS];
		size_t len = j - start;
		if (len == 0 || len >= sizeof(name)){
			return false;
		}
		memcpy(name, &line[start], len);
		name[len] = '\0';

		const char *value = getenv(name);
		if (!append_string(scratch, scratch_size, out, value)){
			return false;
		}
		*index = j + 1;
		return true;
	}

	if (!is_name_start((unsigned char)line[i + 1])){
		if (!append_char(scratch, scratch_size, out, '$')){
			return false;
		}
		*index = i + 1;
		return true;
	}

	size_t j = i + 1;
	while (is_name_char((unsigned char)line[j])){
		++j;
	}

	char name[MAX_ARGS];
	size_t len = j - (i + 1);
	if (len == 0 || len >= sizeof(name)){
		return false;
	}
	memcpy(name, &line[i + 1], len);
	name[len] = '\0';

	const char *value = getenv(name);
	if (!append_string(scratch, scratch_size, out, value)){
		return false;
	}
	*index = j;
	return true;
}

static bool append_plain_token(char *tokens[], int max_tokens, int *count, char *scratch, size_t *out){
	if (*count >= max_tokens - 1){
		return false;
	}
	tokens[*count] = &scratch[*out];
	++(*count);
	return true;
}

static int tokenize(char *line, char *scratch, size_t scratch_size, char *tokens[], int max_tokens){
	size_t out = 0;
	int count = 0;

	for (size_t i = 0; line[i] != '\0' && line[i] != '\n' && line[i] != '\r';){
		unsigned char c = (unsigned char)line[i];

		if (isspace(c)){
			++i;
			continue;
		}

		if (c == '#'){
			break;
		}

		if (c == ';' || c == '<'){
			if (!append_plain_token(tokens, max_tokens, &count, scratch, &out)){
				return -1;
			}
			if (!append_char(scratch, scratch_size, &out, (char)c) || !append_char(scratch, scratch_size, &out, '\0')){
				return -1;
			}
			++i;
			continue;
		}

		if (c == '|'){
			if (!append_plain_token(tokens, max_tokens, &count, scratch, &out)){
				return -1;
			}
			if (line[i + 1] == '|'){
				if (!append_char(scratch, scratch_size, &out, '|') ||
					!append_char(scratch, scratch_size, &out, '|') ||
					!append_char(scratch, scratch_size, &out, '\0')){
					return -1;
				}
				i += 2;
			} else {
				if (!append_char(scratch, scratch_size, &out, '|') ||
					!append_char(scratch, scratch_size, &out, '\0')){
					return -1;
				}
				++i;
			}
			continue;
		}

		if (c == '>'){
			if (!append_plain_token(tokens, max_tokens, &count, scratch, &out)){
				return -1;
			}
			if (line[i + 1] == '>'){
				if (!append_char(scratch, scratch_size, &out, '>') ||
					!append_char(scratch, scratch_size, &out, '>') ||
					!append_char(scratch, scratch_size, &out, '\0')){
					return -1;
				}
				i += 2;
			} else {
				if (!append_char(scratch, scratch_size, &out, '>') ||
					!append_char(scratch, scratch_size, &out, '\0')){
					return -1;
				}
				++i;
			}
			continue;
		}

		if (c == '&'){
			if (line[i + 1] != '&'){
				fprintf(stderr, "mini-shell: unsupported operator '&'\n");
				return -1;
			}
			if (!append_plain_token(tokens, max_tokens, &count, scratch, &out)){
				return -1;
			}
			if (!append_char(scratch, scratch_size, &out, '&') ||
				!append_char(scratch, scratch_size, &out, '&') ||
				!append_char(scratch, scratch_size, &out, '\0')){
				return -1;
			}
			i += 2;
			continue;
		}

		if (!append_plain_token(tokens, max_tokens, &count, scratch, &out)){
			return -1;
		}

		bool in_single = false;
		bool in_double = false;

		while (line[i] != '\0' && line[i] != '\n' && line[i] != '\r'){
			c = (unsigned char)line[i];

			if (!in_single && !in_double){
				if (isspace(c) || c == ';' || c == '<' || c == '>' || c == '|' || c == '&' || c == '#'){
					break;
				}
				if (c == '\''){
					in_single = true;
					++i;
					continue;
				}
				if (c == '"'){
					in_double = true;
					++i;
					continue;
				}
				if (c == '\\'){
					++i;
					if (line[i] == '\0' || line[i] == '\n' || line[i] == '\r'){
						fprintf(stderr, "mini-shell: trailing escape\n");
						return -1;
					}
					if (!append_char(scratch, scratch_size, &out, line[i])){
						return -1;
					}
					++i;
					continue;
				}
				if (c == '$'){
					if (!expand_variable(line, &i, scratch, scratch_size, &out)){
						fprintf(stderr, "mini-shell: invalid variable expansion\n");
						return -1;
					}
					continue;
				}
				if (!append_char(scratch, scratch_size, &out, (char)c)){
					return -1;
				}
				++i;
				continue;
			}

			if (in_single){
				if (c == '\''){
					in_single = false;
					++i;
					continue;
				}
				if (!append_char(scratch, scratch_size, &out, (char)c)){
					return -1;
				}
				++i;
				continue;
			}

			if (in_double){
				if (c == '"'){
					in_double = false;
					++i;
					continue;
				}
				if (c == '\\'){
					++i;
					if (line[i] == '\0' || line[i] == '\n' || line[i] == '\r'){
						fprintf(stderr, "mini-shell: trailing escape\n");
						return -1;
					}
					if (!append_char(scratch, scratch_size, &out, line[i])){
						return -1;
					}
					++i;
					continue;
				}
				if (c == '$'){
					if (!expand_variable(line, &i, scratch, scratch_size, &out)){
						fprintf(stderr, "mini-shell: invalid variable expansion\n");
						return -1;
					}
					continue;
				}
				if (!append_char(scratch, scratch_size, &out, (char)c)){
					return -1;
				}
				++i;
			}
		}

		if (in_single || in_double){
			fprintf(stderr, "mini-shell: unterminated quote\n");
			return -1;
		}

		if (!append_char(scratch, scratch_size, &out, '\0')){
			return -1;
		}
	}

	tokens[count] = NULL;
	return count;
}

static bool is_redirection_token(const char *token){
	return strcmp(token, "<") == 0 || strcmp(token, ">") == 0 || strcmp(token, ">>") == 0;
}

static int parse_command_slice(char *tokens[], int start, int end, Command commands[], int max_commands){
	int command_count = 0;
	init_command(&commands[0]);

	for (int i = start; i < end; ++i){
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

		if (is_redirection_token(token)){
			if (i + 1 >= end || is_operator_token(tokens[i + 1])){
				fprintf(stderr, "mini-shell: redirection missing target file\n");
				return -1;
			}
			char *filename = tokens[++i];
			if (strcmp(token, "<") == 0){
				commands[command_count].input_file = filename;
			} else {
				commands[command_count].output_file = filename;
				commands[command_count].append_output = strcmp(token, ">>") == 0;
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

static Connector connector_from_token(const char *token){
	if (strcmp(token, "&&") == 0){
		return CONNECTOR_AND;
	}
	if (strcmp(token, "||") == 0){
		return CONNECTOR_OR;
	}
	return CONNECTOR_SEQUENCE;
}

static int parse_jobs(char *tokens[], int token_count, Job jobs[], int max_jobs){
	int job_count = 0;
	int start = 0;

	for (int i = 0; i <= token_count; ++i){
		bool at_end = (i == token_count);
		bool at_connector = !at_end && is_connector_token(tokens[i]);

		if (!at_end && !at_connector){
			continue;
		}

		if (i == start){
			fprintf(stderr, "mini-shell: invalid null command\n");
			return -1;
		}

		if (job_count >= max_jobs){
			fprintf(stderr, "mini-shell: too many chained commands\n");
			return -1;
		}

		init_job(&jobs[job_count]);
		jobs[job_count].command_count = parse_command_slice(tokens, start, i, jobs[job_count].commands, MAX_CMDS);
		if (jobs[job_count].command_count < 0){
			return -1;
		}
		jobs[job_count].connector_after = at_end ? CONNECTOR_END : connector_from_token(tokens[i]);
		++job_count;
		start = i + 1;
	}

	return job_count;
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

static int status_from_wait(int status){
	if (WIFEXITED(status)){
		return WEXITSTATUS(status);
	}
	if (WIFSIGNALED(status)){
		return 128 + WTERMSIG(status);
	}
	return 1;
}

static int save_and_apply_redirections(const Command *cmd, int *saved_stdin, int *saved_stdout){
	*saved_stdin = -1;
	*saved_stdout = -1;

	if (cmd->input_file != NULL){
		*saved_stdin = dup(STDIN_FILENO);
		if (*saved_stdin < 0){
			perror("dup");
			return -1;
		}
		if (!apply_input_redirection(cmd->input_file)){
			close(*saved_stdin);
			*saved_stdin = -1;
			return -1;
		}
	}

	if (cmd->output_file != NULL){
		*saved_stdout = dup(STDOUT_FILENO);
		if (*saved_stdout < 0){
			perror("dup");
			if (*saved_stdin >= 0){
				dup2(*saved_stdin, STDIN_FILENO);
				close(*saved_stdin);
				*saved_stdin = -1;
			}
			return -1;
		}
		if (!apply_output_redirection(cmd->output_file, cmd->append_output)){
			close(*saved_stdout);
			*saved_stdout = -1;
			if (*saved_stdin >= 0){
				dup2(*saved_stdin, STDIN_FILENO);
				close(*saved_stdin);
				*saved_stdin = -1;
			}
			return -1;
		}
	}

	return 0;
}

static void restore_redirections(int saved_stdin, int saved_stdout){
	if (saved_stdin >= 0){
		dup2(saved_stdin, STDIN_FILENO);
		close(saved_stdin);
	}
	if (saved_stdout >= 0){
		dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
	}
}

static bool run_external_command(Command *cmd, int *status_out){
	pid_t pid = fork();
	if (pid < 0){
		perror("fork");
		*status_out = 1;
		return false;
	}

	if (pid == 0){
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

	int status;
	if (waitpid(pid, &status, 0) < 0){
		perror("waitpid");
		*status_out = 1;
		return false;
	}

	*status_out = status_from_wait(status);
	return true;
}

static bool run_builtin(Command *cmd, int *status_out, bool *exit_requested){
	int saved_stdin = -1;
	int saved_stdout = -1;

	if (save_and_apply_redirections(cmd, &saved_stdin, &saved_stdout) < 0){
		*status_out = 1;
		return false;
	}

	int status = 0;
	if (strcmp(cmd->argv[0], "exit") == 0){
		*exit_requested = true;
		status = 0;
	} else if (strcmp(cmd->argv[0], "cd") == 0){
		if (cmd->argc > 2){
			fprintf(stderr, "mini-shell: cd takes at most one argument\n");
			status = 1;
		} else {
			const char *target = cmd->argv[1] != NULL ? cmd->argv[1] : getenv("HOME");
			if (target == NULL || chdir(target) != 0){
				perror("cd");
				status = 1;
			}
		}
	} else if (strcmp(cmd->argv[0], "pwd") == 0){
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd)) == NULL){
			perror("pwd");
			status = 1;
		} else {
			puts(cwd);
		}
	} else {
		status = 1;
	}

	fflush(stdout);
	fflush(stderr);
	restore_redirections(saved_stdin, saved_stdout);
	*status_out = status;
	return true;
}

static bool command_is_builtin(const Command *cmd){
	if (cmd->argc == 0 || cmd->argv[0] == NULL){
		return false;
	}
	return strcmp(cmd->argv[0], "cd") == 0 || strcmp(cmd->argv[0], "exit") == 0 || strcmp(cmd->argv[0], "pwd") == 0;
}

static bool run_single_command(Command *cmd, int *status_out, bool *exit_requested){
	if (command_is_builtin(cmd)){
		return run_builtin(cmd, status_out, exit_requested);
	}
	return run_external_command(cmd, status_out);
}

static void reap_started_children(pid_t pids[], int started){
	for (int i = 0; i < started; ++i){
		if (pids[i] > 0){
			int status;
			while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR){
			}
		}
	}
}

static bool run_pipeline(Command commands[], int command_count, int *status_out){
	pid_t pids[MAX_CMDS];
	int prev_read = -1;
	int started = 0;

	for (int i = 0; i < command_count; ++i){
		int pipefd[2] = {-1, -1};
		if (i < command_count - 1 && pipe(pipefd) < 0){
			perror("pipe");
			if (prev_read != -1){
				close(prev_read);
			}
			reap_started_children(pids, started);
			*status_out = 1;
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
			reap_started_children(pids, started);
			*status_out = 1;
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

		pids[started++] = pid;
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

	int last_status = 0;
	pid_t last_pid = pids[started - 1];

	for (int i = 0; i < started; ++i){
		int status;
		if (waitpid(pids[i], &status, 0) < 0){
			perror("waitpid");
			continue;
		}
		if (pids[i] == last_pid){
			last_status = status_from_wait(status);
		}
	}

	*status_out = last_status;
	return true;
}

static bool run_job(Job *job, int *status_out, bool *exit_requested){
	if (job->command_count == 1){
		return run_single_command(&job->commands[0], status_out, exit_requested);
	}

	for (int i = 0; i < job->command_count; ++i){
		if (command_is_builtin(&job->commands[i])){
			fprintf(stderr, "mini-shell: builtins are only supported outside pipelines\n");
			*status_out = 1;
			return false;
		}
	}

	return run_pipeline(job->commands, job->command_count, status_out);
}

static bool should_run_job(Connector connector, int previous_status){
	switch (connector){
		case CONNECTOR_SEQUENCE:
		case CONNECTOR_END:
			return true;
		case CONNECTOR_AND:
			return previous_status == 0;
		case CONNECTOR_OR:
			return previous_status != 0;
	}
	return true;
}

static int execute_line(char *line, bool *exit_requested){
	char scratch[MAX_INPUT * 4];
	char *tokens[MAX_TOKENS];
	Job jobs[MAX_JOBS];

	int token_count = tokenize(line, scratch, sizeof(scratch), tokens, MAX_TOKENS);
	if (token_count < 0){
		fprintf(stderr, "mini-shell: failed to parse input\n");
		return 1;
	}
	if (token_count == 0){
		return g_last_status;
	}

	int job_count = parse_jobs(tokens, token_count, jobs, MAX_JOBS);
	if (job_count < 0){
		return 1;
	}

	int last_status = g_last_status;
	for (int i = 0; i < job_count; ++i){
		if (i > 0 && !should_run_job(jobs[i - 1].connector_after, last_status)){
			continue;
		}

		int status = 0;
		bool job_exit_requested = false;
		run_job(&jobs[i], &status, &job_exit_requested);
		last_status = status;
		if (job_exit_requested){
			*exit_requested = true;
			break;
		}
	}

	g_last_status = last_status;
	return last_status;
}

#ifndef SHELL_NO_MAIN
int main(void){
	char line[MAX_INPUT];
	bool interactive = isatty(STDIN_FILENO);

	while (1){
		if (interactive){
			printf(MAGENTA "mini-shell> " RESET);
			fflush(stdout);
		}

		if (fgets(line, sizeof(line), stdin) == NULL){
			break;
		}

		bool exit_requested = false;
		execute_line(line, &exit_requested);
		if (exit_requested){
			break;
		}
	}

	return 0;
}
#endif
