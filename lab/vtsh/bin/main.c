#include <vtsh.h>
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
                if (interactive) {
                    printf("\n");
                }
                break;
            } else {
                perror("fgets");
                continue;
            }
        }
        
        // Обработка многострочного ввода
        char *lines[100];
        int line_count = 0;
        
        // Разбиваем входные данные по символам \n
        char input_copy[1024];
        strncpy(input_copy, input, sizeof(input_copy) - 1);
        input_copy[sizeof(input_copy) - 1] = '\0';
        
        char *line = strtok(input_copy, "\n");
        while (line != NULL && line_count < 100) {
            lines[line_count++] = line;
            line = strtok(NULL, "\n");
        }
        
        // Обрабатываем каждую строку отдельно
        for (int i = 0; i < line_count; i++) {
            char *current_line = lines[i];
            
            // Пропуск пустых строк
            if (current_line == NULL || strlen(current_line) == 0) {
                continue;
            }
            
            // Убираем пробелы по краям
            char *trimmed = current_line;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
            char *trim_end = trimmed + strlen(trimmed) - 1;
            while (trim_end > trimmed && (*trim_end == ' ' || *trim_end == '\t')) trim_end--;
            *(trim_end + 1) = '\0';
            
            if (strlen(trimmed) == 0) {
                continue;
            }

            // Специальная обработка для команды cat без аргументов
            if (strcmp(trimmed, "cat") == 0) {
                // Читаем stdin до EOF и выводим его содержимое
                char buffer[1024];
                while (fgets(buffer, sizeof(buffer), stdin) != NULL) {
                    // Убираем символ новой строки если нужно
                    buffer[strcspn(buffer, "\n")] = 0;
                    printf("%s\n", buffer);
                    fflush(stdout);
                }
                continue;
            }
            
            // Проверка на фоновое выполнение
            int background = 0;
            char *bg_check = trimmed + strlen(trimmed) - 1;
            
            if (bg_check >= trimmed && *bg_check == '&') {
                background = 1;
                *bg_check = '\0';
                // Убираем пробелы перед &
                while (bg_check > trimmed && (*(bg_check-1) == ' ' || *(bg_check-1) == '\t')) {
                    bg_check--;
                    *bg_check = '\0';
                }
                // Еще раз убираем пробелы по краям
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                trim_end = trimmed + strlen(trimmed) - 1;
                while (trim_end > trimmed && (*trim_end == ' ' || *trim_end == '\t')) trim_end--;
                *(trim_end + 1) = '\0';
            }
            
            // Проверка на пайплайн
            if (strchr(trimmed, '|') != NULL) {
                char ***pipeline_commands = malloc(MAX_PIPES * sizeof(char**));
                int cmd_count = 0;
                
                char pipeline_input[1024];
                strcpy(pipeline_input, trimmed);
                
                if (parse_pipeline(pipeline_input, pipeline_commands, &cmd_count) >= 2) {
                    // Проверяем специальные команды в пайплайне
                    int invalid_special = 0;
                    for (int j = 0; j < cmd_count; j++) {
                        if (pipeline_commands[j] != NULL && pipeline_commands[j][0] != NULL && 
                            is_special_command(pipeline_commands[j][0])) {
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
                    for (int j = 0; j < cmd_count; j++) {
                        if (pipeline_commands[j] != NULL) {
                            for (int k = 0; pipeline_commands[j][k] != NULL; k++) {
                                free(pipeline_commands[j][k]);
                            }
                            free(pipeline_commands[j]);
                        }
                    }
                    free(pipeline_commands);
                    continue;
                }
                free(pipeline_commands);
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
            for (int j = 0; j < arg_count; j++) {
                if (args[j] != NULL && strcmp(args[j], "||") == 0) {
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
            execute_command(args, arg_count, redir_ops, redir_files, redir_count, background);
        }
    }
    
    // Завершение фоновых процессов при выходе
    if (interactive && bg_process_count > 0) {
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