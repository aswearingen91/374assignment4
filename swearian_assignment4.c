#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define INPUT_LENGTH 2048
#define MAX_ARGS     512
#define MAX_BG       200

struct command_line
{
	char *argv[MAX_ARGS + 1];
	char *input_file;
	char *output_file;
	int argc;
	bool is_bg;
};

volatile sig_atomic_t fg_only = 0;
pid_t bg_pids[MAX_BG];
int bg_count = 0;

void handle_SIGTSTP(int signo)
{
	if (fg_only == 0) {
		char *msg = "\nEntering foreground-only mode (& is now ignored)\n";
		write(STDOUT_FILENO, msg, strlen(msg));
		fg_only = 1;
	} else {
		char *msg = "\nExiting foreground-only mode\n";
		write(STDOUT_FILENO, msg, strlen(msg));
		fg_only = 0;
	}
}

void setup_parent_signals(void)
{
	struct sigaction SIGINT_action = {0};
	struct sigaction SIGTSTP_action = {0};

	SIGINT_action.sa_handler = SIG_IGN;
	sigfillset(&SIGINT_action.sa_mask);
	sigaction(SIGINT, &SIGINT_action, NULL);

	/* No SA_RESTART, so Ctrl-Z interrupts fgets and we re-prompt */
	SIGTSTP_action.sa_handler = handle_SIGTSTP;
	sigfillset(&SIGTSTP_action.sa_mask);
	SIGTSTP_action.sa_flags = 0;
	sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

/* Holds the Ctrl-Z message until a foreground child finishes */
void set_SIGTSTP_blocked(bool block)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGTSTP);
	sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}

struct command_line *parse_input(void)
{
	char input[INPUT_LENGTH];
	struct command_line *cmd = calloc(1, sizeof(struct command_line));
	bool last_was_amp = false;

	printf(": ");
	fflush(stdout);

	/* NULL if interrupted by Ctrl-Z, treat as a blank line */
	if (fgets(input, INPUT_LENGTH, stdin) == NULL) {
		clearerr(stdin);
		return cmd;
	}

	char *token = strtok(input, " \n");
	while (token) {
		last_was_amp = false;
		if (!strcmp(token, "<")) {
			cmd->input_file = strdup(strtok(NULL, " \n"));
		} else if (!strcmp(token, ">")) {
			cmd->output_file = strdup(strtok(NULL, " \n"));
		} else if (cmd->argc < MAX_ARGS) {
			cmd->argv[cmd->argc++] = strdup(token);
			if (!strcmp(token, "&"))
				last_was_amp = true;
		}
		token = strtok(NULL, " \n");
	}

	/* Only a final & means background */
	if (last_was_amp) {
		free(cmd->argv[--cmd->argc]);
		cmd->argv[cmd->argc] = NULL;
		cmd->is_bg = true;
	}
	return cmd;
}

void free_command(struct command_line *cmd)
{
	for (int i = 0; i < cmd->argc; i++)
		free(cmd->argv[i]);
	free(cmd->input_file);
	free(cmd->output_file);
	free(cmd);
}

void print_status(int status)
{
	if (WIFEXITED(status))
		printf("exit value %d\n", WEXITSTATUS(status));
	else
		printf("terminated by signal %d\n", WTERMSIG(status));
	fflush(stdout);
}

void check_background(void)
{
	int i = 0;
	int status;

	while (i < bg_count) {
		pid_t done = waitpid(bg_pids[i], &status, WNOHANG);
		if (done > 0) {
			printf("background pid %d is done: ", done);
			print_status(status);
			bg_pids[i] = bg_pids[--bg_count];
		} else {
			i++;
		}
	}
}

void kill_background(void)
{
	for (int i = 0; i < bg_count; i++) {
		kill(bg_pids[i], SIGTERM);
		waitpid(bg_pids[i], NULL, 0);
	}
	bg_count = 0;
}

void redirect(char *path, int target_fd, int flags, char *err_word)
{
	int fd = open(path, flags, 0644);
	if (fd == -1) {
		printf("cannot open %s for %s\n", path, err_word);
		fflush(stdout);
		exit(1);
	}
	dup2(fd, target_fd);
	close(fd);
}

void run_external(struct command_line *cmd, int *last_status)
{
	int status;

	set_SIGTSTP_blocked(true);
	pid_t pid = fork();

	if (pid == 0) {
		struct sigaction SIGINT_action = {0};
		struct sigaction SIGTSTP_action = {0};

		/* Foreground child dies on Ctrl-C, background child ignores it */
		SIGINT_action.sa_handler = cmd->is_bg ? SIG_IGN : SIG_DFL;
		sigaction(SIGINT, &SIGINT_action, NULL);

		SIGTSTP_action.sa_handler = SIG_IGN;
		sigaction(SIGTSTP, &SIGTSTP_action, NULL);
		set_SIGTSTP_blocked(false);

		if (cmd->input_file)
			redirect(cmd->input_file, STDIN_FILENO, O_RDONLY, "input");
		else if (cmd->is_bg)
			redirect("/dev/null", STDIN_FILENO, O_RDONLY, "input");

		if (cmd->output_file)
			redirect(cmd->output_file, STDOUT_FILENO,
			         O_WRONLY | O_CREAT | O_TRUNC, "output");
		else if (cmd->is_bg)
			redirect("/dev/null", STDOUT_FILENO, O_WRONLY, "output");

		execvp(cmd->argv[0], cmd->argv);
		perror(cmd->argv[0]);
		exit(1);
	}

	if (cmd->is_bg) {
		set_SIGTSTP_blocked(false);
		printf("background pid is %d\n", pid);
		fflush(stdout);
		if (bg_count < MAX_BG)
			bg_pids[bg_count++] = pid;
		return;
	}

	waitpid(pid, &status, 0);
	*last_status = status;

	if (WIFSIGNALED(status)) {
		printf("terminated by signal %d\n", WTERMSIG(status));
		fflush(stdout);
	}
	set_SIGTSTP_blocked(false);
}

int main(void)
{
	struct command_line *cmd;
	int last_status = 0;

	setup_parent_signals();

	while (true) {
		check_background();
		cmd = parse_input();

		/* Blank line or comment */
		if (cmd->argc == 0 || cmd->argv[0][0] == '#') {
			free_command(cmd);
			continue;
		}

		if (fg_only)
			cmd->is_bg = false;

		if (!strcmp(cmd->argv[0], "exit")) {
			free_command(cmd);
			kill_background();
			exit(0);
		} else if (!strcmp(cmd->argv[0], "cd")) {
			char *dir = cmd->argc > 1 ? cmd->argv[1] : getenv("HOME");
			chdir(dir);
		} else if (!strcmp(cmd->argv[0], "status")) {
			print_status(last_status);
		} else {
			run_external(cmd, &last_status);
		}

		free_command(cmd);
	}
	return EXIT_SUCCESS;
}
