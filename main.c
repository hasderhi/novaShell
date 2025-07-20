#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>



// ======= INPUT / AUTOCOMPLETE / HISTORY =======

#define MAX_INPUT 256
#define HISTORY_SIZE 50
#define CMD_MAX_LEN 256

int complete_command(char *buffer) {
    if (strncmp(buffer, "history", 7) == 0) {
        strcpy(buffer, "nvs-history");
        return 1;
    }
    if (strncmp(buffer, "pingg", 5) == 0) {
        strcpy(buffer, "ping google.com");
        return 1;
    }
    if (strncmp(buffer, "about", 5) == 0) {
        strcpy(buffer, "nvs-about");
        return 1;
    }
    if (strncmp(buffer, "kill", 4) == 0) {
        strcpy(buffer, "taskkill /PID ");
        return 1;
    }
    // Three letters
    if (strncmp(buffer, "nvs", 3) == 0) {
        strcpy(buffer, "nvs-help");
        return 1;
    }
    if (strncmp(buffer, "sys", 3) == 0) {
        strcpy(buffer, "systeminfo");
        return 1;
    }
    // Two Letters
    if (strncmp(buffer, "di", 2) == 0) {
        strcpy(buffer, "dir");
        return 1; // completed
    }
    if (strncmp(buffer, "..", 2) == 0) {
        strcpy(buffer, "cd ..");
        return 1;
    }
    if (strncmp(buffer, "ip", 2) == 0) {
        strcpy(buffer, "ipconfig");
        return 1;
    }
    if (strncmp(buffer, "co", 2) == 0) {
        strcpy(buffer, "copy");
        return 1;
    }
    if (strncmp(buffer, "mk", 2) == 0) {
        strcpy(buffer, "mkdir");
        return 1;
    }
    if (strncmp(buffer, "ex", 2) == 0) {
        strcpy(buffer, "explorer .");
        return 1;
    }

    // Everything with just one letter goes here
    if (strncmp(buffer, "d", 1) == 0) {
        strcpy(buffer, "del");
        return 1;
    }
    if (strncmp(buffer, "c", 1) == 0) {
        strcpy(buffer, "cls");
        return 1;
    }
    if (strncmp(buffer, "m", 1) == 0) {
        strcpy(buffer, "move");
        return 1;
    }
    if (strncmp(buffer, "r", 1) == 0) {
        strcpy(buffer, "rmdir");
        return 1;
    }
    if (strncmp(buffer, "h", 1) == 0) {
        strcpy(buffer, "help");
        return 1;
    }
    if (strncmp(buffer, "p", 1) == 0) {
        strcpy(buffer, "pwd");
        return 1;
    }
    if (strncmp(buffer, "v", 1) == 0) {
        strcpy(buffer, "ver");
        return 1;
    }
    if (strncmp(buffer, "/", 1) == 0) {
        strcpy(buffer, "cd \\");
        return 1;
    }




    return 0; // no completion
}


static char history[HISTORY_SIZE][CMD_MAX_LEN];
static int history_count = 0;
static int history_index = -1;

void history_add(const char *cmd) {
    if (strlen(cmd) == 0) return; // if cmd empty
    if (history_count > 0 && strcmp(history[(history_count - 1) % HISTORY_SIZE], cmd) == 0)
        return; // no duplicate

    strncpy(history[history_count % HISTORY_SIZE], cmd, CMD_MAX_LEN - 1);
    history[history_count % HISTORY_SIZE][CMD_MAX_LEN - 1] = '\0';
    history_count++;
}

const char* history_get(int direction) {
    if (history_count == 0) return NULL;

    if (direction == -1 && history_index < history_count - 1)
        history_index++;
    else if (direction == 1 && history_index > 0)
        history_index--;
    else if (direction == 1 && history_index == 0) {
        history_index = -1;
        return ""; // Empty line
    }

    if (history_index >= 0)
        return history[(history_count - 1 - history_index) % HISTORY_SIZE];

    return "";
}

void history_print() {
    int start = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
    for (int i = start; i < history_count; i++) {
        printf("%d: %s\n", i - start + 1, history[i % HISTORY_SIZE]);
    }
}


void custom_input(char *buffer, int size) {
    int pos = 0;
    int len = 0;
    history_index = -1;
    buffer[0] = '\0';

    while (1) {
        int ch = _getch();

        if (ch == '\r') {   // Enter
            buffer[len] = '\0';
            printf("\n");
            history_add(buffer);
            return;
        }
        else if (ch == '\t') {  // Tab (autocomplete)
            if (complete_command(buffer)) {
                printf("\r");
                char cwd[MAX_PATH];
                GetCurrentDirectory(MAX_PATH, cwd);
                printf("%s> %s", cwd, buffer);
                pos = len = strlen(buffer);
                fflush(stdout);
            }
        }
        else if (ch == 8) {  // Backspace
            if (pos > 0) {
                memmove(&buffer[pos - 1], &buffer[pos], len - pos);
                pos--;
                len--;
                buffer[len] = '\0';
                printf("\b");
                printf("%s ", &buffer[pos]);
                for (int i = 0; i <= len - pos; i++) printf("\b");
            }
        }
        else if (ch == 0 || ch == 0xE0) {  // Special keys
            ch = _getch();
            if (ch == 0x48) {  // UP arrow
                const char *cmd = history_get(-1);
                if (cmd) {
                    // Clear current line
                    while (pos > 0) { printf("\b \b"); pos--; }
                    strcpy(buffer, cmd);
                    printf("%s", buffer);
                    pos = len = strlen(buffer);
                }
            }
            else if (ch == 0x50) {  // DOWN arrow
                const char *cmd = history_get(1);
                if (cmd) {
                    while (pos > 0) { printf("\b \b"); pos--; }
                    strcpy(buffer, cmd);
                    printf("%s", buffer);
                    pos = len = strlen(buffer);
                }
            }
            else if (ch == 0x4B) {  // LEFT arrow
                if (pos > 0) {
                    printf("\033[D");
                    pos--;
                }
            }
            else if (ch == 0x4D) {  // RIGHT arrow
                if (pos < len) {
                    printf("\033[C");
                    pos++;
                }
            }
            else if (ch == 0x47) {  // Home
                while (pos > 0) {
                    printf("\033[D");
                    pos--;
                }
            }
            else if (ch == 0x4F) {  // End
                while (pos < len) {
                    printf("\033[C");
                    pos++;
                }
            }
        }
        else if (pos < size - 1 && isprint(ch)) {  // Regular chars
            memmove(&buffer[pos + 1], &buffer[pos], len - pos);
            buffer[pos] = (char)ch;
            len++;
            pos++;
            buffer[len] = '\0';
            printf("%s", &buffer[pos - 1]);
            for (int i = 0; i < len - pos; i++) printf("\033[D");
        }
    }
}



// ======= CTRL-C HANDLING =======

volatile int cancel_requested = 0;

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    if (fdwCtrlType == CTRL_C_EVENT) {
        cancel_requested = 1;
        printf("\n^C\n");
        return TRUE; // Don't terminate
    }
    return FALSE;
}


// ======= ALIASES =======
struct Alias {
    const char *alias;
    const char *command;
} aliases[] = {
    {"ls", "dir"},
    {"list", "dir"},
    {"clr", "cls"},
    {"date", "time"},
    {"pwd", "chdir"},
    {NULL, NULL}
};

// ======= ALIAS SYSTEM =======
void apply_aliases(char *cmd) {
    for (int i = 0; aliases[i].alias != NULL; i++) {
        // Only match if first word
        if (strncmp(cmd, aliases[i].alias, strlen(aliases[i].alias)) == 0 &&
            (cmd[strlen(aliases[i].alias)] == ' ' || cmd[strlen(aliases[i].alias)] == '\0')) {
            // Replace alias
            char newCmd[256];
            snprintf(newCmd, sizeof(newCmd), "%s%s",
                     aliases[i].command,
                     cmd + strlen(aliases[i].alias));
            strcpy(cmd, newCmd);
            break;
            }
    }
}


// ======= STRING UTILITIES =======
char *trim(char *str) {
    char *end;
    while (*str == ' ' || *str == '\t') str++;                  // Trim left
    if (*str == 0) return str;                                  // All spaces
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t')) end--;   // Trim right
    *(end + 1) = '\0';
    return str;
}


// ======= CLI =======

void set_color(WORD color) {
    SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
}

void print_exit_message(DWORD code) {
    if (code == 0) { // success
        set_color(FOREGROUND_GREEN);
        printf("Process exited with code 0 (Success).\n");
    } else if (code == 1) { // General fail
        set_color(FOREGROUND_RED);
        printf("Process exited with code 1 (General Error).\n");
    } else if (code == 0xC000013A) {  // Ctrl+C
        set_color(FOREGROUND_RED);
        printf("Process terminated by Ctrl+C (Exit code: 0xC000013A).\n");
    } else { // other
        set_color(FOREGROUND_GREEN | FOREGROUND_RED);
        printf("Process exited with code %lu (0x%lX).\n", code, code);
    }
    set_color(FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN);
}

void builtin_cls(void) {
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    DWORD count;
    COORD homeCoords = {0, 0};
    if (!GetConsoleScreenBufferInfo(hConsole, &csbi)) return;
    FillConsoleOutputCharacter(hConsole, ' ', csbi.dwSize.X * csbi.dwSize.Y, homeCoords, &count);
    FillConsoleOutputAttribute(hConsole, csbi.wAttributes, csbi.dwSize.X * csbi.dwSize.Y, homeCoords, &count);
    SetConsoleCursorPosition(hConsole, homeCoords);
}




// ======= BUILT-IN COMMANDS =======

// Built-in 'dir'

void builtin_dir(const char *path) {
    WIN32_FIND_DATA findData;
    HANDLE hFind;
    char searchPath[MAX_PATH];
    SYSTEMTIME stUTC, stLocal;
    char timeBuf[32];

    if (path == NULL || strlen(path) == 0) {
        snprintf(searchPath, sizeof(searchPath), "*");
    } else {
        snprintf(searchPath, sizeof(searchPath), "%s\\*", path);
    }

    hFind = FindFirstFile(searchPath, &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        printf("No files found or cannot open directory.\n");
        return;
    }

    printf("Directory listing for %s:\n\n", (path && strlen(path)) ? path : ".");

    do {
        // make last write time readable
        FileTimeToSystemTime(&findData.ftLastWriteTime, &stUTC);
        SystemTimeToTzSpecificLocalTime(NULL, &stUTC, &stLocal);
        sprintf(timeBuf, "%02d/%02d/%04d %02d:%02d",
            stLocal.wDay, stLocal.wMonth, stLocal.wYear,
            stLocal.wHour, stLocal.wMinute);

        // Set color based on file type
        WORD color = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; // White
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { // dir
            color = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        } else {
            char *ext = strrchr(findData.cFileName, '.');
            if (ext && (_stricmp(ext, ".exe") == 0 || _stricmp(ext, ".bat") == 0)) { // .exe / .bat
                color = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            }
            if (ext && (_stricmp(ext, ".zip") == 0 || _stricmp(ext, ".tar.gz") == 0)) { // .zip / .tar.gz
                color = FOREGROUND_RED | FOREGROUND_INTENSITY;
            }
        }

        set_color(color);

        // File size (skip for dir)
        LARGE_INTEGER fileSize;
        fileSize.HighPart = findData.nFileSizeHigh;
        fileSize.LowPart = findData.nFileSizeLow;

        printf("%-20s  %10lld  %s\n",
            findData.cFileName,
            (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : fileSize.QuadPart,
            timeBuf);

        set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); // Reset

    } while (FindNextFile(hFind, &findData) != 0);

    FindClose(hFind);
    printf("\n");
}

// Built-in 'cd'
void builtin_cd(const char *path) {
    if (path == NULL || strlen(path) == 0) {
        printf("Usage: cd <directory>\n");
        return;
    }
    if (!SetCurrentDirectory(path)) {
        printf("Failed to change directory (Error %lu).\n", GetLastError());
    }
}

// Built-in 'nvs-help' (Help)
void builtin_nvs_help(void) {
    set_color(FOREGROUND_BLUE | FOREGROUND_RED);
    printf("\n"
           "novaShell 0.0.1-alpha\n"
           "For more info, please visit https://tk-dev-software.com/novaShell\n\n");
    set_color(FOREGROUND_GREEN | FOREGROUND_BLUE);
    printf("====== HELP ======\n"
        "Built-in commands:\n"
        "   - cd\n"
        "   - dir\n"
        "   - nvs-help\n"
        "   - nvs-about\n"
        "   - nvs-history\n"
        "   - exit\n"
        "\n"
        "Aliases:\n"
        "   - ls > dir\n"
        "   - list > dir\n"
        "   - clr > cls\n"
        "   - date > time\n"
        "   - pwd > chdir\n");
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// Built-in 'nvs-about'
void builtin_nvs_about(void) {
    set_color(FOREGROUND_BLUE | FOREGROUND_RED);
    printf("\n"
           "novaShell 0.0.1-alpha\n"
           "By Tobias Kisling ('hasderhi', 'tk_dev')\n"
           "For more info, please visit https://tk-dev-software.com/novaShell\n\n");

    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}

// ======= MAIN SHELL =======
int main(void) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    SetConsoleTitle("novaShell");

    char input[256];
    int helpCmd = 0;

    set_color(FOREGROUND_BLUE | FOREGROUND_RED);
    printf("novaShell 0.0.1-alpha\nType 'exit' to quit.\n");
    set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);

    while (1) {
        helpCmd = 0;

        char cwd[MAX_PATH];
        GetCurrentDirectory(MAX_PATH, cwd);

        set_color(FOREGROUND_INTENSITY);
        printf("%s> ", cwd);

        custom_input(input, sizeof(input));

        set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);


        char *cmd = trim(input);
        if (strlen(cmd) == 0) continue;  // Empty input

        if (cancel_requested) {
            cancel_requested = 0;
            continue;  // Back to prompt
        }

        if (strcmp(cmd, "exit") == 0) {
            break;
        }

        SetConsoleTitle(cmd);

        apply_aliases(cmd);

        // Built-in commands
        if (strncmp(cmd, "cd", 2) == 0 && (cmd[2] == ' ' || cmd[2] == '\0')) {
            builtin_cd(trim(cmd + 2));
            SetConsoleTitle("novaShell");
            continue;
        }
        if (strncmp(cmd, "dir", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
            builtin_dir(trim(cmd + 3));
            SetConsoleTitle("novaShell");
            continue;
        }
        if (strncmp(cmd, "nvs-help", 8) == 0 && (cmd[8] == ' ' || cmd[8] == '\0')) {
            builtin_nvs_help();
            SetConsoleTitle("novaShell");
            continue;
        }
        if (strncmp(cmd, "nvs-about", 9) == 0 && (cmd[9] == ' ' || cmd[9] == '\0')) {
            builtin_nvs_about();
            SetConsoleTitle("novaShell");
            continue;
        }
        if (strncmp(cmd, "nvs-history", 11) == 0 && (cmd[11] == ' ' || cmd[11] == '\0')) {
            history_print();
            SetConsoleTitle("novaShell");
            continue;
        }
        if (strncmp(cmd, "history", 7) == 0 && (cmd[7] == ' ' || cmd[11] == '\0')) {
            // Because I don't want to reserve a common term like 'history' that could be used by another program.
            set_color(FOREGROUND_BLUE | FOREGROUND_RED);
            printf("\n To show novaShell history, type 'nvs-history'\n\n");
            set_color(FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN);
            SetConsoleTitle("novaShell");
        }
        if (strncmp(cmd, "help", 4) == 0 && (cmd[4] == ' ' || cmd[4] == '\0')) {
            helpCmd = 1;
        }
        if (strncmp(cmd, "cls", 3) == 0 && (cmd[3] == ' ' || cmd[3] == '\0')) {
            builtin_cls();
            SetConsoleTitle("novaShell");
            continue;
        }

        // Execute external command
        char fullCmd[512];
        snprintf(fullCmd, sizeof(fullCmd), "cmd.exe /C %s", cmd);

        STARTUPINFO si = {0};
        PROCESS_INFORMATION pi = {0};
        si.cb = sizeof(si);

        if (!CreateProcess(NULL, fullCmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            printf("CreateProcess failed (%lu).\n", GetLastError());
            continue;
        }

        WaitForSingleObject(pi.hProcess, INFINITE);

        if (helpCmd) {
            set_color(FOREGROUND_BLUE | FOREGROUND_RED);
            printf("\nFor dedicated novaShell help, type 'nvs-help'\n\n");
            set_color(FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN);
        } else {
            DWORD exitCode;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            print_exit_message(exitCode);
        }

        SetConsoleTitle("novaShell");
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    set_color(FOREGROUND_BLUE | FOREGROUND_RED);
    printf("novaShell exited.\n");
    set_color(FOREGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN);
    return 0;
}
