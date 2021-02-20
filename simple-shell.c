/**
 * A simple shell implementing some basic shell operations
 * Supports:
 * 	1.	File Input/Output redirection via < and >
 * 	2.	Program output -> Program input redirection via |
 * 	3.	Command history via !!
 * 	4.	Concurrent execution via &
 * 
 * Also does basic shell stuff, like executing programs
 * To quit, type exit()
 */

#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>


// ----------- IMPORTANT --------------
#define MAX_ARG 40 // Maximum of 40 args per command
// ------------------------------------


// Splits a line  along spaces,
// generating an array of args. Also gives arg count.
size_t splitArgs(char* line_src, char** args) {

	char line_cpy[BUFSIZ];
	char* token = NULL, * delim = " ";
	size_t count = 0;

	// Create a tmp copy of line_buf_src that can be modified
	strcpy(line_cpy, line_src);

	// Split into args via the delimiter
	for(char* token = strtok(line_cpy, delim); 
			token != NULL; token = strtok(NULL, delim)) {
		
		if(count < MAX_ARG - 1)
			args[count++] = token;
		else {
			fprintf(stderr, "Command exceeds the argument limit!"
					"Cannot fully interpret.");
			break;
		} 
		

	}

	return count;
}


/* Executes the command contain in "args", forking
 * the executed command into a seperate process.
 * 
 * Use "_wait" to control whether the shell should
 * wait for the command process to finish.
*/ 
void forkInto(char** args, bool _wait) {

	// Perform the fork
	pid_t pid = fork();

	switch(pid) {
	case -1:
		fprintf(stderr,"Failed to fork process\n");
		break;
	case 0: // child
		if(execvp(args[0], args) == -1)
			fprintf(stderr, "Chould not find a program named %s\n", args[0]);
		fflush(stdout);
		fflush(stderr);
		break;
	default: // parent
		if(_wait)
			wait(NULL); // wait for child
		break;
	}
		
}

/* Redirects file desc. "dest" to file desc. "source"
 * Closes the current file desc. "dest" as a consequence
 * Sets the "error" bool to true if an error occurs
 * Returns a copy of the file desc. "dest" from before it closed
 */
int redirect(int source, int dest, bool* error) {

	// Duplicate the stream being modified so it can be restored
	int std_tmp_copy = dup(dest);

	// Perform the rediction
	if(dup2(source, dest) == -1) {
		fprintf(stderr, "Failed to redirect input/output.\n");
		(*error) = true;
	}
	return std_tmp_copy;
}


/* Redirects file desc. "dest" to file at path "file"
 * Closes the current file desc. "dest" as a consequence
 * Sets the "error" bool to true if an error occurs
 * Returns a copy of the file desc. "dest" from before it closed
 */
int redirectToFile(char* file, int dest, bool* error) {
	
	int fd, std_tmp_copy = -1;

	// Check for null file
	if(file == NULL) {
		fprintf(stderr, "Please specify a file to redirect into!\n");
		(*error) = true;
		return std_tmp_copy;
	}

	// Get the file in question with appropriate perms
	if(dest == STDIN_FILENO) {
		if((fd = open(file, O_RDONLY, S_IRUSR | S_IWUSR)) == -1)  {
			fprintf(stderr, "Failed to open file %s!\n", file);
			(*error) = true;
			return std_tmp_copy;
		}
	} else {
		if((fd = creat(file, S_IRUSR | S_IWUSR)) == -1)  {
			fprintf(stderr, "Failed to read/create file %s!\n", file);
			(*error) = true;
			return std_tmp_copy;
		}
	}

	// Perform the redirection
	std_tmp_copy = redirect(fd, dest, error);

	// File can be closed after redirection.
	close(fd);

	return std_tmp_copy;

}

/* Executes the command contain in "args", forking
 * the executed command into a seperate process.
 * The executed program's output is then used as the
 * input to execute "dest_prog" via a pipe.
 * 
 * Use "_wait" to control whether the shell should
 * wait for the command process to finish.
 */ 
void forkAndPipeInto(char** args, char* dest_prog, bool _wait) {
	
	// Perform the fork
	pid_t pid = fork(), piped_pid;
	int pipefd[2];
	bool error = false;
	int stdin_reset, stdout_reset;

	switch(pid) {
	case -1: // failed to fork
		fprintf(stderr,"Failed to fork process\n");
		break;

	case 0: // child

		// Establish the pipe
		if(pipe(pipefd) == -1) {
			fprintf(stderr, "Failed to establish a pipe between the processes!");
			return;
		}

		// Child will execute "dest_prog", but yet another process 
		// is needed to execute the original command in "args." Fork
		// again to create that extra process.
		piped_pid = fork();

		switch(piped_pid) {
		case -1: // failed to fork
			fprintf(stderr,"Failed to fork process\n");
			break; 

		case 0: // child (grandchild of original process)
		
			close(pipefd[0]); // child will not read, only write

			// redirect stdout to pipe's write end
			stdout_reset = redirect(pipefd[1], STDOUT_FILENO, &error);
			if(!error) {

				// execute command from args
				if(execvp(args[0], args) == -1)
					fprintf(stderr, "Chould not find a program named %s\n", args[0]);	
				
				dup2(stdout_reset, STDOUT_FILENO); // close stdout, then restore 
			
			}

			break;

		default: // parent (child of original process)

			close(pipefd[1]); // parent will not write, only read

			wait(NULL); // wait for child prog to execute

			// redirect stdin to pipe's read end
			stdin_reset = redirect(pipefd[0], STDIN_FILENO, &error);
			if(!error) {

				// execute program
				if(execlp(dest_prog, dest_prog, NULL) == -1)
					fprintf(stderr, "Chould not find a program named %s\n", args[0]);

				dup2(stdin_reset, STDIN_FILENO); // close stdin, then restore

			}

			fflush(stdout);
			fflush(stderr);

			break;
		}
	
		break;

	default: // parent
		if(_wait)
			wait(NULL); // wait for child

		break;
	}

}

/* 
 * Searchs an array of arguments, seperating commands from
 * control characters. After fully parsing the args, execute
 * the commands that were discovered while honoring the control
 * characters specified
 */
void interpretArgs(char** args, size_t arg_count) {

	 // Calloc initializes everything as NULL automatically
	 // arg_count + 1 to ensure a null exists.
	char** exec_args = calloc(arg_count + 1, sizeof(char*));
	int exec_arg_count = 0;

	// Have to defer all execution until the entire command is parsed
	// Use flags to dynamically modify the execution type as the command
	// is interpreted.
	bool wait = true, error = false, redirected = false, pipe = false;
	int redirected_from, redirected_to;
	char* pipe_prog = NULL;

	// Parse until all args are consumed or error
	for(int arg = 0; arg < arg_count && !error; ++arg) {

		// Special Modifier: < or > (redirect flags)
		if(args[arg][0] == '<' || args[arg][0] == '>') {

			// As per the rubric, only one redirection is supported
			if(redirected) {
				fprintf(stderr, "Multiple redirects in a single command unsupported!");
				error = true;
			} else {
				redirected = true;
				redirected_to = (args[arg][0] == '<')  ? STDIN_FILENO : STDOUT_FILENO;
				redirected_from = redirectToFile(args[++arg], redirected_to, &error);
			}

		// Special Modifier: | (pipeline flag)
		} else if(args[arg][0] == '|') {

			// Only one pipe is supported
			if(pipe) {
				fprintf(stderr, "Multiple pipes in a single command unsupported!");
				error = true;
			} else {
				pipe = true;
				pipe_prog = args[++arg];
			}

		// Special Modifier: & (run-in-parallel flag)
		} else if(args[arg][0] == '&') {
			wait = false;

		// Standard Case: Pass arg to exec() for interpreting
		} else
			exec_args[exec_arg_count++] = args[arg];
	

	}
	
	// Execute the command (if no error occured)
	if(!error) {
		if(pipe)
			forkAndPipeInto(exec_args, pipe_prog, wait);
		else
			forkInto(exec_args, wait);
	}

	// Sub_args is dynamic, needs to be deallocated
	free(exec_args);

	// Restore stdout and stdin, if they were redirected
	if(redirected) {
		dup2(redirected_from, redirected_to);
		close(redirected_from);
	}

}

int main(void)
{

	char line_buf[BUFSIZ];
	char last_line_buf[BUFSIZ];
	char* args[MAX_ARG];	
	size_t arg_count = 0;

	// Ensure all memory is initilized to NULL
	memset(line_buf, 0, BUFSIZ * sizeof(char));
	memset(args, 0, MAX_ARG * sizeof(char*));
	
	while (true){   // while(true) -> Run until a break occurs
		printf("osh>");
		fflush(stdout);

		// Read current command and split
		fgets(line_buf, BUFSIZ, stdin);

		// Get line doesn't delete the delimiting \n.
		// Do that manually.
		(*strrchr(line_buf, '\n')) = 0;

		arg_count = splitArgs(line_buf, args);

		if(args[0] == NULL || strcmp(args[0], "") == 0) {
			fprintf(stderr, "Please enter a command!\n");
			fflush(stderr);
		} else {
			
			// Special Command: exit or exit()
			if(strcmp(args[0], "exit()") == 0 ||
					strcmp(args[0], "exit") == 0)
				break;  // break loop to exit

			// Special Command: !!
			if(strcmp(args[0], "!!") == 0) {

				// Load last command if present
				if(strlen(last_line_buf) == 0) {
					// Abort, no history!!
					fprintf(stderr, "No commands in history\n");
					fflush(stderr);
					continue;

				} else // Change args to last command's args
					arg_count = splitArgs(last_line_buf, args);

			} else // New command; update command history
				strcpy(last_line_buf, line_buf);

			interpretArgs(args, arg_count);

		} // VALID COMMAND IF 

	} // WHILE(TRUE)
    
	return 0;
}
