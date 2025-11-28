#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <linux/sched.h>
#include <sys/syscall.h>
#include <limits.h>

#define MAX_ARGS 64
#define MAX_PIPES 10
#define MAX_BG_PROCESSES 100

const char* vtsh_prompt() {
    return "vtsh> ";
}

// Глобальные переменные для фоновых процессов
pid_t background_processes[MAX_BG_PROCESSES];
int bg_process_count = 0;

// Сигнальные обработчики
void handle_signal(int sig) {
    if (sig == SIGINT) {
        printf("\n%s", vtsh_prompt());
        fflush(stdout);
        return;
    }
    if (sig == SIGQUIT) {
        exit(EXIT_SUCCESS);
    }
}

// Очистка завершенных фоновых процессов
void cleanup_background_processes() {
    for (int i = 0; i < bg_process_count; ) {
        int status;
        pid_t result = waitpid(background_processes[i], &status, WNOHANG);
        if (result > 0) {
            printf("[Background process %d finished with status %d]\n", 
                   background_processes[i], WEXITSTATUS(status));
            // Сдвигаем массив
            for (int j = i; j < bg_process_count - 1; j++) {
                background_processes[j] = background_processes[j + 1];
            }
            bg_process_count--;
        } else if (result == -1) {
            // Удаляем несуществующий PID
            for (int j = i; j < bg_process_count - 1; j++) {
                background_processes[j] = background_processes[j + 1];
            }
            bg_process_count--;
        } else {
            i++;
        }
    }
}

// Добавление фонового процесса
void add_background_process(pid_t pid) {
    if (bg_process_count < MAX_BG_PROCESSES) {
        background_processes[bg_process_count++] = pid;
        printf("[Background process started with PID: %d]\n", pid);
    }
}

// Специальные команды
int exec_spec_commands(char **argv) {
    if (strcmp(argv[0], "cd") == 0) {
        char *path = getenv("HOME");
        
        if (argv[1] == NULL) {
            argv[1] = path;
        }
        if (chdir(argv[1]) != 0) {
            perror("chdir");
            return 0;
        }
        return 1;
    } 
    else if (strcmp(argv[0], "export") == 0) {
        if (argv[1] == NULL || strchr(argv[1], '=') == NULL) {
            fprintf(stderr, "export: invalid argument\n");
            return 0;
        }
        if (putenv(argv[1]) != 0) {
            fprintf(stderr, "Error setting environment variable: %s\n", argv[1]);
            return 0;
        }
        return 1;
    } 
    else if (strcmp(argv[0], "unset") == 0) {
        if (argv[1] == NULL) {
            fprintf(stderr, "unset: missing argument\n");
            return 0;
        }
        if (unsetenv(argv[1]) != 0) {
            fprintf(stderr, "Error unsetting environment variable: %s\n", argv[1]);
            return 0;
        }
        return 1;
    }
    else if (strcmp(argv[0], "pwd") == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            printf("%s\n", cwd);
            return 1;
        } else {
            perror("pwd");
            return 0;
        }
    }
    return 0; // Не специальная команда
}

// Проверка является ли команда специальной
int is_special_command(const char *cmd) {
    return strcmp(cmd, "cd") == 0 || 
           strcmp(cmd, "export") == 0 || 
           strcmp(cmd, "unset") == 0 ||
           strcmp(cmd, "pwd") == 0;
}

// Применение перенаправлений в дочернем процессе
void apply_redirections(char **redir_ops, char **redir_files, int redir_count) {
    for (int i = 0; i < redir_count; i++) {
        if (strcmp(redir_ops[i], ">") == 0) {
            int fd = open(redir_files[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd == -1) {
                perror("open");
                exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2");
                close(fd);
                exit(1);
            }
            close(fd);
        } 
        else if (strcmp(redir_ops[i], ">>") == 0) {
            int fd = open(redir_files[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd == -1) {
                perror("open");
                exit(1);
            }
            if (dup2(fd, STDOUT_FILENO) == -1) {
                perror("dup2");
                close(fd);
                exit(1);
            }
            close(fd);
        } 
        else if (strcmp(redir_ops[i], "<") == 0) {
            int fd = open(redir_files[i], O_RDONLY);
            if (fd == -1) {
                perror("open");
                exit(1);
            }
            if (dup2(fd, STDIN_FILENO) == -1) {
                perror("dup2");
                close(fd);
                exit(1);
            }
            close(fd);
        }
    }
}

// Разбиение строки на аргументы с обработкой перенаправлений
int parse_args(char *input, char **args, char **redir_ops, char **redir_files, int *redir_count) {
    char *token;
    int count = 0;
    *redir_count = 0;
    
    token = strtok(input, " \t");
    while (token != NULL && count < MAX_ARGS - 1) {
        // Проверка на операторы перенаправления
        if (strcmp(token, ">") == 0 || strcmp(token, ">>") == 0 || strcmp(token, "<") == 0) {
            if (*redir_count < MAX_ARGS / 2) {
                redir_ops[*redir_count] = token;
                // Следующий токен должен быть файлом
                token = strtok(NULL, " \t");
                if (token == NULL) {
                    fprintf(stderr, "Syntax error: missing filename for %s\n", redir_ops[*redir_count]);
                    return -1;
                }
                redir_files[*redir_count] = token;
                (*redir_count)++;
            }
        } else {
            args[count++] = token;
        }
        token = strtok(NULL, " \t");
    }
    args[count] = NULL;
    
    return count;
}

// Создание процесса через sys_clone3
pid_t create_process() {
    struct clone_args args = {};
    args.flags = 0;
    args.exit_signal = SIGCHLD;
    return syscall(SYS_clone3, &args, sizeof(args));
}

// Выполнение одной команды
int execute_command(char **args, int arg_count, char **redir_ops, char **redir_files, int redir_count, int background) {
    if (arg_count == 0) {
        return 0;
    }
    
    // Проверка на exit
    if (strcmp(args[0], "exit") == 0) {
        exit(EXIT_SUCCESS);
    }
    
    // Проверка на специальные команды
    if (is_special_command(args[0])) {
        if (redir_count > 0) {
            fprintf(stderr, "Special commands do not support redirections\n");
            return 1;
        }
        return exec_spec_commands(args) ? 0 : 1;
    }
    
    // Создание процесса через clone3
    pid_t pid = create_process();
    if (pid == -1) {
        perror("clone3");
        return 1;
    }
    
    if (pid == 0) {
        // Дочерний процесс
        if (redir_count > 0) {
            apply_redirections(redir_ops, redir_files, redir_count);
        }
        
        execvp(args[0], args);
        perror("execvp");
        exit(1);
    }
    
    // Родительский процесс
    if (background) {
        add_background_process(pid);
        return 0;
    } else {
        int status;
        waitpid(pid, &status, 0);
        
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        } else {
            return 1;
        }
    }
}

// Разбор пайплайна (на основе C++ логики)
int parse_pipeline(char *input, char ***pipeline_commands, int *command_count) {
    // Создаем копию входной строки
    char input_copy[1024];
    strncpy(input_copy, input, sizeof(input_copy) - 1);
    input_copy[sizeof(input_copy) - 1] = '\0';
    
    // Временные массивы для хранения команд
    char *commands[MAX_PIPES];
    int count = 0;
    
    // Разбиваем по '|' аналогично C++ коду
    char *current_command = input_copy;
    char *pipe_pos;
    
    while ((pipe_pos = strchr(current_command, '|')) != NULL && count < MAX_PIPES - 1) {
        // Выделяем команду до пайпа
        *pipe_pos = '\0';
        
        // Убираем пробелы вокруг команды
        char *cmd_start = current_command;
        while (*cmd_start == ' ' || *cmd_start == '\t') cmd_start++;
        char *cmd_end = cmd_start + strlen(cmd_start) - 1;
        while (cmd_end > cmd_start && (*cmd_end == ' ' || *cmd_end == '\t')) cmd_end--;
        *(cmd_end + 1) = '\0';
        
        // Сохраняем команду если она не пустая
        if (strlen(cmd_start) > 0) {
            commands[count] = strdup(cmd_start);
            count++;
        }
        
        // Переходим к следующей части
        current_command = pipe_pos + 1;
    }
    
    // Обрабатываем последнюю команду
    if (strlen(current_command) > 0 && count < MAX_PIPES) {
        // Убираем пробелы вокруг команды
        char *cmd_start = current_command;
        while (*cmd_start == ' ' || *cmd_start == '\t') cmd_start++;
        char *cmd_end = cmd_start + strlen(cmd_start) - 1;
        while (cmd_end > cmd_start && (*cmd_end == ' ' || *cmd_end == '\t')) cmd_end--;
        *(cmd_end + 1) = '\0';
        
        if (strlen(cmd_start) > 0) {
            commands[count] = strdup(cmd_start);
            count++;
        }
    }
    
    // Парсим каждую команду в отдельные аргументы
    for (int i = 0; i < count; i++) {
        pipeline_commands[i] = malloc(MAX_ARGS * sizeof(char*));
        if (!pipeline_commands[i]) {
            perror("malloc");
            // Освобождаем уже выделенную память
            for (int j = 0; j < i; j++) {
                free(pipeline_commands[j]);
            }
            for (int j = 0; j < count; j++) {
                free(commands[j]);
            }
            return -1;
        }
        
        // Инициализируем NULL
        for (int j = 0; j < MAX_ARGS; j++) {
            pipeline_commands[i][j] = NULL;
        }
        
        // Парсим аргументы команды
        int arg_count = 0;
        char *token = strtok(commands[i], " \t");
        while (token != NULL && arg_count < MAX_ARGS - 1) {
            pipeline_commands[i][arg_count++] = strdup(token);
            token = strtok(NULL, " \t");
        }
        pipeline_commands[i][arg_count] = NULL;
        
        // Освобождаем временную строку команды
        free(commands[i]);
    }
    
    *command_count = count;
    return count;
}

// Выполнение пайплайна (на основе C++ логики)
int execute_pipeline(char ***commands, int command_count) {
    if (command_count < 2) {
        fprintf(stderr, "Invalid pipeline syntax\n");
        return 1;
    }
    
    int pipes[command_count - 1][2];
    pid_t pids[command_count];
    
    // Создаем пайпы
    for (int i = 0; i < command_count - 1; i++) {
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            return 1;
        }
    }
    
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    // Запускаем команды
    for (int i = 0; i < command_count; i++) {
        pids[i] = create_process();
        
        if (pids[i] == -1) {
            perror("clone3");
            return 1;
        }
        
        if (pids[i] == 0) {
            // Настройка пайпов (как в C++ коде)
            if (i > 0) {
                dup2(pipes[i-1][0], STDIN_FILENO);
            }
            if (i < command_count - 1) {
                dup2(pipes[i][1], STDOUT_FILENO);
            }
            
            // Закрываем все пайпы (как в C++ коде)
            for (int j = 0; j < command_count - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            
            // Выполняем команду
            execvp(commands[i][0], commands[i]);
            perror("execvp");
            exit(1);
        }
    }
    
    // Закрываем пайпы в родительском процессе
    for (int i = 0; i < command_count - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    
    // Ждем завершения всех процессов
    int status = 0;
    for (int i = 0; i < command_count; i++) {
        pid_t result;
        do {
            result = waitpid(pids[i], &status, 0);
        } while (result == -1 && errno == EINTR);
        
        if (result == -1) {
            perror("waitpid");
        }
    }
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + 
                     (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
    
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else {
        return 1;
    }
}

// Обработка логического OR
int handle_or_command(char **args, int arg_count) {
    int or_pos = -1;
    
    // Ищем оператор ||
    for (int i = 0; i < arg_count; i++) {
        if (strcmp(args[i], "||") == 0) {
            or_pos = i;
            break;
        }
    }
    
    if (or_pos == -1 || or_pos == 0 || or_pos >= arg_count - 1) {
        fprintf(stderr, "||: invalid syntax. Usage: command1 || command2\n");
        return 1;
    }
    
    // Разделяем команды
    char **first_cmd = malloc(MAX_ARGS * sizeof(char*));
    char **second_cmd = malloc(MAX_ARGS * sizeof(char*));
    
    int first_count = 0;
    for (int i = 0; i < or_pos; i++) {
        first_cmd[first_count++] = args[i];
    }
    first_cmd[first_count] = NULL;
    
    int second_count = 0;
    for (int i = or_pos + 1; i < arg_count; i++) {
        second_cmd[second_count++] = args[i];
    }
    second_cmd[second_count] = NULL;
    
    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    
    int exit_code = execute_command(first_cmd, first_count, NULL, NULL, 0, 0);
    
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    long elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
    
    if (exit_code != 0) {
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        exit_code = execute_command(second_cmd, second_count, NULL, NULL, 0, 0);
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
    }
    
    free(first_cmd);
    free(second_cmd);
    
    return exit_code;
}

// Основной цикл shell
int main() {
    char input[1024];
    int interactive = isatty(STDIN_FILENO);
    
    // Установка обработчиков сигналов
    signal(SIGINT, handle_signal);
    signal(SIGQUIT, handle_signal);
    
    while (1) {
        cleanup_background_processes();
        
        if (interactive) {
            printf("%s", vtsh_prompt());
            fflush(stdout);
        }
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            if (feof(stdin)) {
                // Ctrl+D
                printf("\n");
                break;
            } else {
                perror("fgets");
                continue;
            }
        }
        
        // Удаление символа новой строки
        input[strcspn(input, "\n")] = 0;
        
        // Пропуск пустых строк
        if (strlen(input) == 0) {
            continue;
        }
        
        // Обработка escape-последовательностей (\n -> n)
        char cleaned_input[1024];
        int clean_index = 0;
        for (int i = 0; input[i] != '\0'; i++) {
            if (input[i] == '\\' && input[i + 1] != '\0') {
                cleaned_input[clean_index++] = input[++i];
            } else {
                cleaned_input[clean_index++] = input[i];
            }
        }
        cleaned_input[clean_index] = '\0';
        
        // Убираем пробелы по краям
        char *trimmed = cleaned_input;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        char *trim_end = trimmed + strlen(trimmed) - 1;
        while (trim_end > trimmed && (*trim_end == ' ' || *trim_end == '\t')) trim_end--;
        *(trim_end + 1) = '\0';
        
        if (strlen(trimmed) == 0) {
            continue;
        }
        
        // Проверка на фоновое выполнение
        int background = 0;
        if (trim_end > trimmed && *trim_end == '&') {
            background = 1;
            *trim_end = '\0';
            // Убираем пробелы перед &
            while (trim_end > trimmed && (*(trim_end-1) == ' ' || *(trim_end-1) == '\t')) trim_end--;
            *trim_end = '\0';
        }
        
        // Проверка на пайплайн
        if (strchr(trimmed, '|') != NULL) {
            char **pipeline_commands[MAX_PIPES];  // Исправлено: char ** вместо char *
            int cmd_count = 0;
            
            char pipeline_input[1024];
            strcpy(pipeline_input, trimmed);
            
            if (parse_pipeline(pipeline_input, pipeline_commands, &cmd_count) >= 2) {
                // Проверяем специальные команды в пайплайне
                int invalid_special = 0;
                for (int i = 0; i < cmd_count; i++) {
                    if (pipeline_commands[i] != NULL && pipeline_commands[i][0] != NULL && 
                        is_special_command(pipeline_commands[i][0])) {
                        fprintf(stderr, "Special commands cannot be used in pipeline\n");
                        invalid_special = 1;
                        break;
                    }
                }
                
                if (!invalid_special) {
                    if (background) {
                        fprintf(stderr, "Background execution not supported for pipelines\n");
                    } else {
                        execute_pipeline(pipeline_commands, cmd_count);
                    }
                }
                
                // Освобождаем память
                for (int i = 0; i < cmd_count; i++) {
                    free(pipeline_commands[i]);
                }
                continue;
            }
        }
        
        // Обычная команда
        char *args[MAX_ARGS];
        char *redir_ops[MAX_ARGS / 2];
        char *redir_files[MAX_ARGS / 2];
        int redir_count = 0;
        
        char command_input[1024];
        strcpy(command_input, trimmed);
        
        int arg_count = parse_args(command_input, args, redir_ops, redir_files, &redir_count);
        if (arg_count <= 0) {
            continue;
        }
        
        // Проверка на логическое OR
        int has_or = 0;
        for (int i = 0; i < arg_count; i++) {
            if (strcmp(args[i], "||") == 0) {
                has_or = 1;
                break;
            }
        }
        
        if (has_or) {
            if (background) {
                fprintf(stderr, "Background execution not supported for || operator\n");
            } else {
                handle_or_command(args, arg_count);
            }
            continue;
        }
        
        // Выполнение команды
        struct timespec start_time, end_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        
        int exit_code = execute_command(args, arg_count, redir_ops, redir_files, redir_count, background);
        
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        
        if (!background && interactive) {
            long elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000 + (end_time.tv_nsec - start_time.tv_nsec) / 1000000;
            // Можно вывести время выполнения если нужно
        }
    }
    
    // Завершение фоновых процессов при выходе
    if (interactive) {
        printf("Waiting for background processes...\n");
        for (int i = 0; i < bg_process_count; i++) {
            int status;
            kill(background_processes[i], SIGTERM);
            sleep(1);
            if (waitpid(background_processes[i], &status, WNOHANG) == 0) {
                kill(background_processes[i], SIGKILL);
                waitpid(background_processes[i], &status, 0);
            }
        }
    }
    
    return 0;
}