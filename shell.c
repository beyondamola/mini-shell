#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <stdbool.h>

#define MAX_ARGS 64
#define MAX_INPUT 128

#define RESET "\x1b[0m"
#define MAGENTA "\x1b[35m"
void tokenize_V(char** args, char line[], int* argl){
	*argl = 0; 
	int i = 0;

	// clear the arguments list
	for (i = 0; i < MAX_ARGS; ++i){
	args[i] = NULL;
	}

	// tokenize and instantiate arguments using strtok()
	char* token = strtok(line, " \t\r\n");
	while(token != NULL && *argl < MAX_ARGS - 1){
		args[*argl] = token;
		(*argl)++;
		token = strtok(NULL, " \t\r\n");
	}
	args[*argl] = NULL;
}

bool run_command(char* arg, char** args){
	pid_t pid = fork();
	if (pid == -1) {perror("Fork failed - Reprompt\n"); return false;}

	if (pid == 0) {
	// child process - execvp()
	execvp(arg, args); 
	perror(arg);
	_exit(0);
	}
	else {
	// parent process wait() logic
	int status; 
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)){perror("Child Process Error - Signal"); return false;}
	}
	return true;
}

int main (){
	char line[MAX_INPUT];
	char* args[MAX_ARGS];
	int argl = 0;

	while(1){
	printf(MAGENTA "mini-shell> " RESET);

	// Ctl + D signal
	if(fgets(line, sizeof(line), stdin) == NULL){break;}
	tokenize_V(args, line, &argl);

	//safety checks for args
	if (args[0] == NULL){continue;} 

	// checks for exit signal
	if (strcmp(args[0], "exit") == 0){break;}

	// change directory bulitin
	if (strcmp(args[0], "cd") == 0){chdir(args[1]); continue;}
	
	run_command(args[0], args);

	}
	return 0;
}
