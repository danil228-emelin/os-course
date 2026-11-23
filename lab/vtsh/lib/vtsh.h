#pragma once

#include <sys/types.h>
#include <signal.h>

#define MAX_ARGS 64
#define MAX_PIPES 10
#define MAX_BG_PROCESSES 100

// Функции shell
const char* vtsh_prompt(void);
void handle_signal(int sig);
void cleanup_background_processes(void);
void add_background_process(pid_t pid);

// Специальные команды
int exec_spec_commands(char **argv);
int is_special_command(const char *cmd);

// Перенаправления
void apply_redirections(char **redir_ops, char **redir_files, int redir_count);
int parse_args(char *input, char **args, char **redir_ops, char **redir_files, int *redir_count);

// Управление процессами
pid_t create_process(void);
int execute_command(char **args, int arg_count, char **redir_ops, char **redir_files, int redir_count, int background);

// Пайплайны
int parse_pipeline(char *input, char ***pipeline_commands, int *command_count);
int execute_pipeline(char ***commands, int command_count);


extern pid_t background_processes[MAX_BG_PROCESSES];
extern int bg_process_count;

// Логические операторы
int handle_or_command(char **args, int arg_count);
